#include "resident.hpp"
#include "local_pipe.hpp"
#include "protocol.hpp"
#include "../message_store.hpp"
#include <dafeiyu/digest.hpp>
#include <nlohmann/json.hpp>
#include <random>
#include <array>
#include <utility>
#include <windows.h>

namespace dfy::management {
namespace {
using Json = nlohmann::json;
using app::SoftwareEvent;
using app::SoftwareResult;

// 校验管道回包的版本与业务结果，只有正式结果进入调用方。
Json decode(std::string_view bytes) {
    auto response = Json::parse(bytes);
    if (response.at("v")!=1) throw std::runtime_error("management: incompatible reply");
    if (response.contains("error")) throw std::runtime_error("management: "+response.at("error").get<std::string>());
    return std::move(response.at("result"));
}

// 在相同的用户管道内区分管理命令与业务消息，失效的客户端须重新建立观察身份。
// 步骤：核对版本与操作 → 处理连接、查询或退出 → 将有效客户端的业务请求交给统一协议。
Json handle(Host& host, const std::string& identity, const std::string& instance, StopSignal& intent, bool& stopping, const Json& request, app::SoftwareLog* log) {
    if (!request.is_object() || request.at("v")!=1) throw std::invalid_argument("invalid protocol");
    const auto& op = request.at("op").get_ref<const std::string&>();
    if (op=="status" && (request.size()==2 || (request.size()==3 && request.contains("instance")))) {
        if (request.contains("instance") && request.at("instance")!=instance) throw std::invalid_argument("host instance differs");
        Json status{{"state",stopping ? "stopping" : host.dialogue().display ? "running" : "starting"},
            {"instance",instance},{"pid",GetCurrentProcessId()},{"definition",identity}};
        if (stopping) {
            constexpr std::array phases{"requested","conversation","plugins","complete","failed"};
            const auto progress = host.stop_progress();
            const auto elapsed = progress.phase==StopPhase::requested ? 0 : std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now()-progress.since).count();
            status["progress"] = {{"stage",phases.at(static_cast<std::size_t>(progress.phase))},{"elapsedMs",elapsed},
                {"session",std::to_string(progress.run.session)},{"turn",std::to_string(progress.run.sequence)}};
            status["operation"] = "stop-"+instance;
        }
        return status;
    }
    if (op=="stop" && request.size()==3) {
        if (request.at("instance")!=instance) throw std::invalid_argument("host instance differs");
        if (intent.accept()) {
            stopping = true;
            if (log) log->record(SoftwareEvent::stop_accepted,SoftwareResult::accepted,{.instance=instance,.shutdown=true});
            host.close(true);
        }
        return {{"state","stopping"},{"instance",instance},{"operation","stop-"+instance}};
    }
    if (stopping) throw std::runtime_error("host is stopping");
    if (op=="connect" && request.size()==3) {
        if (request.at("definition")!=identity) throw std::invalid_argument("host configuration differs");
        const auto client = host.open_client();
        if (log) log->record(SoftwareEvent::client_connected,SoftwareResult::ok,{.instance=instance,.client=client});
        return {{"client",client},{"instance",instance},{"pid",GetCurrentProcessId()},{"stopWatch",true}};
    }

    const auto& client = request.at("client").get_ref<const std::string&>();
    if (!host.visit_client(client)) throw std::invalid_argument("client expired");
    if (op=="disconnect" && request.size()==3) {
        host.close_client(client);
        if (log) log->record(SoftwareEvent::client_disconnected,SoftwareResult::ok,{.instance=instance,.client=client});
        return {{"state","disconnected"}};
    }
    if (op=="request" && request.size()==4) return dispatch(&host,request.at("body"),client);
    if (op=="terminal" && request.size()==4) return host.terminal().exchange(client,request.at("body"));
    throw std::invalid_argument("unknown operation");
}
}

// 规范化启动路径并计算标识，配置内容和凭据不进入握手。
std::string startup_identity(const std::filesystem::path& model, const std::filesystem::path& packages,
    const std::filesystem::path& trace) {
    Json paths = Json::array();
    for (const auto& path : {model,packages,trace}) {
        const auto value = path.empty() ? std::u8string{} : std::filesystem::weakly_canonical(std::filesystem::absolute(path)).generic_u8string();
        paths.push_back(std::string(value.begin(),value.end()));
    }
    return platform::sha256(paths.dump());
}

// 先取得端点再创建应用，退出期间继续回答状态查询并等待工作实际结束。
// 步骤：取得目录和端点 → 启动本体 → 首次就绪后挂接可选托盘并服务请求 → 业务退出后释放托盘。
int serve_resident(const std::filesystem::path& profile, const std::string& identity, Host::Open open,
    std::stop_token stop, app::SoftwareLog* log, StartResidentTray start_tray) {
    ProfileLease lease(profile,log);
    LocalPipe pipe(profile);
    std::random_device random;
    const auto instance = std::to_string(random())+"-"+std::to_string(random())+"-"+std::to_string(random());
    StopSignal intent(instance);
    if (log) {
        log->bind(instance);
        log->record(SoftwareEvent::host_listening,SoftwareResult::ok);
    }
    // 即使管道异常离开，逆序析构也先等待业务线程，再释放托盘与目录所有权。
    std::unique_ptr<ResidentTray> tray;
    std::shared_ptr<app::MessageStore> messages;
    try { messages = std::make_shared<app::MessageStore>(profile); }
    catch (...) {
        if (log) log->record(SoftwareEvent::message_storage,SoftwareResult::failed);
        throw;
    }
    Host host(std::move(open),{},log,std::move(messages),instance);
    bool stopping = false;
    while (!host.exited()) {
        if (stop.stop_requested() && !stopping) { stopping = true; host.close(); }
        if (start_tray && !stopping && host.dialogue().display) {
            auto start = std::exchange(start_tray,nullptr);
            try { tray = start(profile,instance,log); }
            catch (const std::system_error& error) {
                if (log) log->record(SoftwareEvent::tray_start,SoftwareResult::failed,{.system_error=error.code().value()});
            } catch (...) {
                if (log) log->record(SoftwareEvent::tray_start,SoftwareResult::failed);
            }
        }
        pipe.serve_one(std::chrono::milliseconds(100),[&](const std::string& input) {
            try { return Json{{"v",1},{"result",handle(host,identity,instance,intent,stopping,Json::parse(input),log)}}.dump(); }
            catch (const Json::exception&) { return Json{{"v",1},{"error","invalid management message"}}.dump(); }
            catch (const std::invalid_argument& error) { return Json{{"v",1},{"error",error.what()}}.dump(); }
            catch (const std::runtime_error&) { return Json{{"v",1},{"error","management operation unavailable"}}.dump(); }
        });
    }
    return host.failed() ? 1 : 0;
}

// 在独立连接内交换一条请求，显式校验回包版本和业务拒绝。
nlohmann::json resident_request(const std::filesystem::path& profile, Json request) {
    request["v"] = 1;
    return decode(local_request(profile,request.dump()));
}

// 在管道确认前取得原进程和显式退出事件的观察权，兼容只返回身份的旧后台。
// 步骤：提交配置握手 → 从管道对端绑定原内核对象 → 完成传输确认并发布连接。
ResidentConnection connect_resident(const std::filesystem::path& profile, const std::string& definition) {
    const Json request{{"v",1},{"op","connect"},{"definition",definition}};
    ResidentConnection result;
    std::exception_ptr failure;
    (void)local_request(profile,request.dump(),std::chrono::seconds(5),[&](void* process, std::uint32_t pid, std::string_view reply) {
        try {
            const auto value = decode(reply);
            if (value.contains("pid") && value.at("pid")!=pid) throw std::runtime_error("management: handshake process differs");
            result.instance = value.at("instance");
            result.client = value.at("client");
            result.watch = std::make_shared<ResidentWatch>(process,pid,result.instance,value.value("stopWatch",false));
        } catch (...) { failure = std::current_exception(); }
    });
    if (failure) std::rethrow_exception(failure);
    return result;
}

// 只向握手或查询所得的原实例申请退出，分别记录提交和已核对的回执。
// 步骤：记录提交身份 → 交换原协议并核对接纳 → 记录耗时或保留有界系统错误。
Json request_stop(const std::filesystem::path& profile, const std::string& instance, std::string_view client, app::SoftwareLog* log) {
    using namespace std::chrono;
    app::SoftwareFields fields{.instance=instance,.client=client,.shutdown=true};
    const auto start = steady_clock::now();
    if (log) log->record(SoftwareEvent::stop_submitted,SoftwareResult::started,fields);
    try {
        auto response = resident_request(profile,{{"op","stop"},{"instance",instance}});
        if (response.at("instance")!=instance || response.at("state")!="stopping") throw std::runtime_error("management: invalid stop reply");
        fields.duration_ms = duration_cast<milliseconds>(steady_clock::now()-start).count();
        if (log) log->record(SoftwareEvent::stop_reply,SoftwareResult::accepted,fields);
        return response;
    } catch (const std::exception& error) {
        if (const auto* system = dynamic_cast<const std::system_error*>(&error)) fields.system_error = system->code().value();
        fields.duration_ms = duration_cast<milliseconds>(steady_clock::now()-start).count();
        if (log) log->record(SoftwareEvent::stop_reply,SoftwareResult::failed,fields);
        throw;
    }
}
}
