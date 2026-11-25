#include "client.hpp"
#include "protocol.hpp"
#include "poll_log.hpp"
#include "resident.hpp"
#include <windows.h>
#include <algorithm>

namespace dfy::management {
namespace {
using Json = nlohmann::json;
using namespace std::chrono_literals;

// 将一个 Windows 参数编码为 CRT 可原样解码的引号字符串。
std::wstring quote(const std::wstring& value) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t c : value) {
        if (c==L'\\') { ++slashes; continue; }
        if (c==L'"') result.append(slashes+1,L'\\');
        result.append(slashes,L'\\');
        result += c;
        slashes = 0;
    }
    result.append(slashes*2,L'\\');
    return result+L"\"";
}
}

// 在初次缺少端点时启动本体，只有尚未完成的启动才允许继续等候握手。
// 步骤：尝试原配置握手 → 按系统错误决定启动或报告 → 有界等候端点。
ResidentConnection connect_or_launch(std::stop_token stop, const std::filesystem::path& profile, const std::string& definition,
    const Client::Launch& start, app::SoftwareLog* log) {
    const auto end = std::chrono::steady_clock::now()+6s;
    bool launched = false;
    while (!stop.stop_requested()) {
        try { return connect_resident(profile,definition); }
        catch (const std::system_error& error) {
            if (error.code().value()!=ERROR_FILE_NOT_FOUND && error.code().value()!=ERROR_PATH_NOT_FOUND) throw;
            if (!launched) {
                start();
                launched = true;
                if (log) log->record(app::SoftwareEvent::host_launch,app::SoftwareResult::accepted);
            }
            if (std::chrono::steady_clock::now()>=end) throw;
            std::this_thread::sleep_for(50ms);
        }
    }
    throw std::runtime_error("client closed");
}

namespace {
// 用已绑定的进程描述退出事实，收尾中的等待对象只从同一实例读回。
// 步骤：固定内核采样 → 在原进程仍运行时补读收尾阶段 → 管道不可用时保留已核实的事实。
Json lifecycle(const std::filesystem::path& profile, const std::string& instance, const ResidentObservation& peer,
    const Json& request, PollLog& polls) {
    Json result{{"event","host.lifecycle"},{"version","1"},{"request",request.at("request")},
        {"requested",peer.requested},{"pid",peer.pid},{"exitCode",nullptr}};
    if (peer.exit_code) result["exitCode"] = *peer.exit_code;
    else {
        try {
            const auto state = resident_request(profile,{{"op","status"},{"instance",instance}});
            if (state.at("instance")!=instance || state.at("pid")!=peer.pid) throw std::runtime_error("management: original peer differs");
            if (state.contains("progress")) result["progress"] = state.at("progress");
            polls.success(app::SoftwareEvent::poll_stop);
        } catch (const std::exception& error) {
            polls.failure(app::SoftwareEvent::poll_stop,error);
            // 管道可能先于进程关闭，保留本次内核采样。
        }
    }
    return result;
}
}

// 由 Windows 直接启动本体程序，子进程不继承窗口或管道句柄。
// 步骤：准备已引用的本地参数 → 隐藏启动 → 归还创建句柄。
void launch_host(const std::filesystem::path& program, const std::filesystem::path& config,
    const std::filesystem::path& packages, const std::filesystem::path& profile, const std::filesystem::path& trace) {
    auto command = quote(program.native())+L" --host "+quote(config.native())+L" --plugins "+quote(packages.native())+
        L" --data "+quote(profile.native());
    if (!trace.empty()) command += L" --trace "+quote(trace.native());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(program.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process))
        throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"management launch");
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

// 在独立线程握手，构造调用方可以继续推进原生窗口。
Client::Client(std::filesystem::path profile, std::string definition, Launch start, Logs logs, app::SoftwareLog* software)
    : software_(software), thread_([this,profile=std::move(profile),definition=std::move(definition),start=std::move(start),logs=std::move(logs)](std::stop_token stop) {
        run(stop,profile,definition,start,logs);
    }) {}

// 唤醒工作线程，jthread 在其他成员销毁之前等待其离开。
Client::~Client() { close(); }

// 按请求数量与待发字节预算接纳观察者请求。
bool Client::post(const Json& request, Reply reply) {
    if (!request.is_object() || !request.contains("type") || !request.at("type").is_string() ||
        !request.contains("request") || !request.at("request").is_string() || !reply) return false;
    const auto bytes = request.dump().size();
    std::lock_guard lock(mutex_);
    if (thread_.get_stop_token().stop_requested() || pending_.size()>=16 || bytes>request_limit ||
        queued_bytes_+bytes>2*request_limit) return false;
    pending_.push_back({request,std::move(reply),bytes});
    queued_bytes_ += bytes;
    changed_.notify_one();
    return true;
}

// 取走结果后在锁外通知原始视图。
void Client::advance() {
    std::deque<Entry> completed;
    {
        std::lock_guard lock(mutex_);
        completed.swap(completed_);
    }
    changed_.notify_one();
    for (auto& entry : completed) entry.deliver(entry.message);
}

// 核对活动观察句柄，旧协议实例还可用本客户端已核实的退出接纳回执。
ResidentObservation Client::observe() const noexcept {
    std::lock_guard lock(mutex_);
    if (!watch_) return {};
    auto observation = watch_->observe();
    observation.requested |= stop_accepted_.load();
    return observation;
}

// 撤销尚未交付的观察读取，已经排队的写命令由工作线程处理。
void Client::close() noexcept {
    std::lock_guard lock(mutex_);
    thread_.request_stop();
    std::erase_if(pending_,[&](const auto& entry) {
        const auto& type = entry.message.at("type").template get_ref<const std::string&>();
        if (!type.ends_with(".read")) return false;
        queued_bytes_ -= entry.bytes;
        return true;
    });
    changed_.notify_one();
}

// 在同一观察身份下顺序交付请求，未知的写入结果留给原命令核对。
// 步骤：取得观察身份 → 分派本窗诊断或后台请求 → 缓存回包并在停止后解除绑定。
void Client::run(std::stop_token stop, const std::filesystem::path& profile, const std::string& definition, const Launch& start, const Logs& logs) {
    std::string client, instance, error;
    PollLog polls(software_);
    try {
        auto connection = connect_or_launch(stop,profile,definition,start,software_);
        client = std::move(connection.client);
        instance = std::move(connection.instance);
        if (software_) {
            software_->bind(instance,client);
            software_->record(app::SoftwareEvent::client_connected,app::SoftwareResult::ok);
        }
        std::lock_guard lock(mutex_);
        watch_ = std::move(connection.watch);
    } catch (const std::exception& ex) {
        error = ex.what();
        app::SoftwareFields fields;
        if (const auto* system = dynamic_cast<const std::system_error*>(&ex)) fields.system_error = system->code().value();
        if (software_) software_->record(app::SoftwareEvent::client_connected,app::SoftwareResult::failed,fields);
    }
    for (;;) {
        Entry item;
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock,[&] { return stop.stop_requested() || (!pending_.empty() && completed_.size()<2); });
            if (pending_.empty() && stop.stop_requested()) break;
            item = std::move(pending_.front());
            pending_.pop_front();
            queued_bytes_ -= item.bytes;
        }
        const auto& type = item.message.at("type");
        std::optional<app::SoftwareEvent> poll;
        if (type=="dialogue.read") poll = app::SoftwareEvent::poll_dialogue;
        else if (type=="room.read") poll = app::SoftwareEvent::poll_room;
        else if (type=="state.read") poll = app::SoftwareEvent::poll_run;
        try {
            const auto peer = type=="dialogue.read" ? observe() : ResidentObservation{};
            if (type=="logs.read" || type=="logs.open") {
                if (!logs) throw std::runtime_error("window diagnostics unavailable");
                auto response = logs(type=="logs.open");
                response.update(Json{{"event","logs.status"},{"version","1"},{"request",item.message.at("request")}});
                item.message = std::move(response);
            } else if (client.empty()) throw std::runtime_error(error);
            else if (!peer.error && (peer.requested || peer.exit_code)) item.message = lifecycle(profile,instance,peer,item.message,polls);
            else if (type=="host.stop") {
                const auto reply = request_stop(profile,instance,client,software_);
                stop_accepted_ = true;
                item.message = {{"event","host.stopping"},{"version","1"},{"request",item.message.at("request")},{"state",reply.at("state")}};
            } else item.message = resident_request(profile,{{"op","request"},{"client",client},{"body",item.message}});
            if (poll) polls.success(*poll);
        } catch (const std::exception& ex) {
            if (poll) polls.failure(*poll,ex);
            item.message = {{"event","management.error"},{"version","1"},{"request",item.message.at("request")},{"reason",ex.what()}};
        }
        std::lock_guard lock(mutex_);
        if (!stop.stop_requested()) completed_.push_back(std::move(item));
    }
    if (!client.empty()) {
        try { (void)resident_request(profile,{{"op","disconnect"},{"client",client}}); } catch (const std::exception&) {}
    }
    exited_ = true;
}
}
