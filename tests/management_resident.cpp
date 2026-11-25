#include "management/resident.hpp"
#include "management/client.hpp"
#include "management/local_pipe.hpp"
#include <windows.h>
#include <nlohmann/json.hpp>
#include <future>
#include <fstream>
#include <iostream>
#include <semaphore>

namespace {
using namespace std::chrono_literals;
using Json = nlohmann::json;
using namespace dfy;

// 有界等待指定状态，服务未发布端点时允许下次重读。
template<class Predicate>
void wait_until(Predicate ready) {
    const auto end = std::chrono::steady_clock::now()+4s;
    do {
        try { if (ready()) return; } catch (const std::system_error&) {}
        std::this_thread::sleep_for(15ms);
    } while (std::chrono::steady_clock::now()<end);
    throw std::runtime_error("resident state timed out");
}

// 从实际客户端提交一项读取或退出，并在有界时间内收取窗口回调。
// 步骤：登记请求与回包位置 → 推进客户端直到回包 → 返回实际数据。
Json window_reply(management::Client& client, const Json& body) {
    auto result = std::make_shared<Json>();
    if (!client.post(body,[result](const Json& value) { *result = value; })) throw std::runtime_error("window request rejected");
    wait_until([&] { client.advance(); return !result->is_null(); });
    return *result;
}

// 使用同号请求查回终端命令，业务尚未执行时管道仍可接纳其他客户端。
Json terminal_reply(const std::filesystem::path& profile, const std::string& client, const Json& body) {
    Json result;
    wait_until([&] {
        result = management::resident_request(profile,{{"op","terminal"},{"client",client},{"body",body}});
        return !result.is_null();
    });
    return result;
}

// 在原实例接纳退出之后断开 IPC，并让同目录的新端点返回自己的等待对象。
// 步骤：完成真实观察握手 → 置位并丢弃退出回包 → 核对独立意图 → 拒绝新实例的收尾数据。
void lost_stop_reply(const std::filesystem::path& root) {
    const auto profile = root/"lost-stop";
    const auto pid = GetCurrentProcessId();
    const auto instance = std::to_string(pid)+"-1-1";
    management::StopSignal signal(instance);
    auto owner = std::make_unique<management::LocalPipe>(profile);
    auto original = std::async(std::launch::async,[&] {
        const bool connected = owner->serve_one(3s,[&](const std::string& bytes) {
            if (Json::parse(bytes).at("op")!="connect") throw std::runtime_error("unexpected initial operation");
            return Json{{"v",1},{"result",{{"client","first"},{"instance",instance},{"pid",pid},{"stopWatch",true}}}}.dump();
        });
        if (!connected) throw std::runtime_error("handshake not acknowledged");
        return owner->serve_one(3s,[&](const std::string& bytes) -> std::string {
            const auto request = Json::parse(bytes);
            if (request.at("op")!="stop" || request.at("instance")!=instance || !signal.accept()) throw std::runtime_error("wrong stop target");
            throw std::system_error(ERROR_BROKEN_PIPE,std::system_category(),"fixture dropped response");
        });
    });
    management::Client client(profile,"fixture",[] { throw std::runtime_error("unexpected launch"); });
    const auto stop = window_reply(client,{{"type","host.stop"},{"version","1"},{"request","original-stop"}});
    const auto observed = client.observe();
    if (original.get() || stop.at("event")!="management.error" || stop.at("request")!="original-stop" ||
        !observed.requested || observed.pid!=pid || observed.exit_code || observed.error) throw std::runtime_error("lost reply lost the original intent");
    owner.reset();

    {
        management::LocalPipe replacement(profile);
        auto exchange = std::async(std::launch::async,[&] {
            return replacement.serve_one(3s,[&](const std::string& bytes) {
                const auto request = Json::parse(bytes);
                if (request.at("op")!="status" || request.at("instance")!=instance) throw std::runtime_error("unbound follow-up");
                return Json{{"v",1},{"result",{{"instance","new-instance"},{"pid",pid},{"progress",{{"stage","plugins"}}}}}}.dump();
            });
        });
        const auto reply = window_reply(client,{{"type","dialogue.read"},{"version","1"},{"request","after-restart"},{"command",""},{"revision",""}});
        if (!exchange.get() || reply.at("event")!="host.lifecycle" || !reply.at("requested").get<bool>() || reply.at("pid")!=pid ||
            !reply.at("exitCode").is_null() || reply.contains("progress")) throw std::runtime_error("replacement progress reached old window");
    }
}

// 托盘不可用时真实后台仍接受握手与显式退出。
// 步骤：模拟无桌面或原生启动失败 → 等待业务就绪 → 通过原管道退出并核对一次启动。
void headless(const std::filesystem::path& root, bool fail) {
    const auto data = root/(fail ? "tray-failed" : "no-desktop");
    std::atomic_uint starts{0};
    std::stop_source stop;
    auto server = std::async(std::launch::async,[&] {
        return management::serve_resident(data,"headless",[&] {
            return std::make_unique<app::Application>(model::Config{},root/"plugins",data,model::Backend{
                [](const auto&,const auto&) { return model::Response{{model::Role::assistant,"fixture"}}; }});
        },stop.get_token(),nullptr,[&](const auto&,const auto&,auto*) -> std::unique_ptr<management::ResidentTray> {
            ++starts;
            if (fail) throw std::system_error(ERROR_NOT_READY,std::system_category());
            return {};
        });
    });
    try {
        wait_until([&] { return starts==1; });
        const auto binding = management::connect_resident(data,"headless");
        const auto reply = management::request_stop(data,binding.instance,binding.client,nullptr);
        if (reply.at("state")!="stopping" || server.get()!=0 || starts!=1) throw std::runtime_error("headless host failed");
    } catch (...) { stop.request_stop(); throw; }
}

// 复查只有一次退出接纳，而且收尾实效包含被阻塞的原回合及实际等待。
// 步骤：按原实例筛选日志 → 核对回合与耗时 → 检查接纳次数及隐私字段。
void verify_stop_log(const std::filesystem::path& data, const std::string& instance, const Json& turn) {
    int accepted = 0;
    bool drained = false;
    for (const auto& file : std::filesystem::directory_iterator(data/"logs")) {
        std::ifstream input(file.path(),std::ios::binary);
        for (std::string line; std::getline(input,line); ) {
            if (line.find("synthetic-private")!=std::string::npos) throw std::runtime_error("private dialogue in process log");
            const auto row = Json::parse(line);
            if (row.value("instance",std::string{})!=instance || row.value("operation",std::string{})!="stop-"+instance) continue;
            if (row.at("event")=="stop.accepted") ++accepted;
            if (row.at("event")=="conversation.drain" && row.at("result")=="ok")
                drained = row.at("durationMs").get<int>()>=80 && row.at("session").dump()==turn.at("session").get<std::string>() &&
                    row.at("turn").dump()==turn.at("turn").get<std::string>();
        }
    }
    if (accepted!=1 || !drained) throw std::runtime_error("stop evidence did not match original work");
}

// 在正式管理客户端交错读取多个主题，核对恢复、换类和关窗各自结算原错误。
// 步骤：启动隔离管道 → 连续注入两类错误及成功回包 → 从软件日志核对计数、时间和保密字段。
void poll_errors(const std::filesystem::path& root) {
    const auto profile = root/("poll-"+std::to_string(GetCurrentProcessId()));
    management::LocalPipe pipe(profile);
    const auto instance = std::to_string(GetCurrentProcessId())+"-3-4";
    unsigned dialogue = 0, room = 0;
    std::jthread server([&](std::stop_token stop) {
        while (!stop.stop_requested()) pipe.serve_one(100ms,[&](const std::string& bytes) -> std::string {
            const auto request = Json::parse(bytes);
            Json reply{{"v",1},{"result",Json::object()}};
            if (request.at("op")=="connect") reply["result"] = {{"instance",instance},{"client","client-1-2-3-4"}};
            if (request.at("op")=="request") {
                const auto& type = request.at("body").at("type");
                if (type=="dialogue.read" && ++dialogue<=2) reply["error"] = "synthetic-private-error";
                if (type=="room.read" && ++room>=2) {
                    if (room==2) reply["error"] = "synthetic-private-room";
                    else return "synthetic-private-malformed";
                }
            }
            return reply.dump();
        });
    });
    {
        app::SoftwareLog software(profile,app::SoftwareRole::window);
        management::Client client(profile,"fixture",[] { throw std::logic_error("unexpected launch"); },{},&software);
        for (const auto type : {"dialogue.read","room.read","dialogue.read","dialogue.read","room.read","room.read","room.read"})
            (void)window_reply(client,{{"type",type},{"version","1"},{"request","synthetic-private-id"}});
    }
    unsigned recovered = 0, ended = 0;
    for (const auto& file : std::filesystem::directory_iterator(profile/"logs")) {
        std::ifstream input(file.path(),std::ios::binary);
        for (std::string line; std::getline(input,line); ) {
            if (line.find("synthetic-private")!=line.npos) throw std::runtime_error("sensitive poll field logged");
            const auto row = Json::parse(line);
            if (!row.at("event").get<std::string>().starts_with("poll.")) continue;
            if (row.at("firstUtcMs")>row.at("lastUtcMs") || row.at("instance")!=instance) throw std::runtime_error("invalid poll samples");
            if (row.at("event")=="poll.dialogue" && row.at("result")=="recovered" && row.at("count")==2) ++recovered;
            if (row.at("event")=="poll.room" && row.at("result")=="ended" &&
                ((row.at("failureKind")=="protocol" && row.at("count")==2) ||
                 (row.at("failureKind")=="management" && row.at("count")==1))) ++ended;
        }
    }
    if (recovered!=1 || ended!=2) throw std::runtime_error("poll intervals not recovered or closed exactly once");
}

// 用析构回执核对业务收尾期间的托盘寿命。
struct TestTray final : management::ResidentTray {
    std::atomic_uint& releases;
    // 绑定独立于后台线程的释放计数。
    explicit TestTray(std::atomic_uint& count) : releases(count) {}
    // 记录原实例实际归还的托盘。
    ~TestTray() override { ++releases; }
};

// 让独立观察者先补读原委托，再在下一回合仍在工作时显式退出。
// 步骤：启动隔离宿主 → 接纳后断开与重连 → 在实际调用退出前核对收尾 → 按原实例复查持久回执。
void reconnect(const std::filesystem::path& root) {
    const auto data = root/"user", plugins = root/"plugins";
    std::filesystem::create_directories(plugins);
    std::binary_semaphore started(0), release(0);
    std::binary_semaphore allow_open(0);
    std::atomic_uint tray_starts{0}, tray_releases{0};
    std::string tray_instance;
    auto server = std::async(std::launch::async,[&] {
        app::SoftwareLog software(data,app::SoftwareRole::host);
        const int code = management::serve_resident(data,"fixture",[&] {
            if (!allow_open.try_acquire_for(5s)) throw std::runtime_error("application startup gate timed out");
            return std::make_unique<app::Application>(model::Config{},plugins,data,model::Backend{
                [&](const auto&,const auto& input) {
                    started.release();
                    if (!release.try_acquire_for(5s)) throw std::runtime_error("gate timed out");
                    return model::Response{{model::Role::assistant,"回复："+input.messages.back().content}};
                }});
        },{},&software,[&](const auto& profile,const std::string& original,app::SoftwareLog* log) {
            if (profile!=data || log!=&software) throw std::runtime_error("wrong tray context");
            tray_instance = original;
            ++tray_starts;
            return std::make_unique<TestTray>(tray_releases);
        });
        return software.exiting(code);
    });
    const auto ask = [&](Json request) { return management::resident_request(data,std::move(request)); };
    std::string instance;
    try {
        wait_until([&] { return ask({{"op","status"}}).at("state")=="starting"; });
        if (tray_starts!=0) throw std::runtime_error("tray started before application readiness");
        allow_open.release();
        wait_until([&] { const auto status=ask({{"op","status"}}); instance=status.at("instance"); return status.at("state")=="running"; });
        wait_until([&] { return tray_starts==1; });
        if (tray_instance!=instance) throw std::runtime_error("tray is bound to a different instance");
        try {
            (void)management::serve_resident(data,"duplicate",{}, {},nullptr,[&](const auto&,const auto&,auto*) {
                ++tray_starts;
                return std::make_unique<TestTray>(tray_releases);
            });
            throw std::logic_error("duplicate owner admitted");
        } catch (const std::runtime_error& error) {
            if (std::string(error.what()).find("already in use")==std::string::npos) throw;
        }
        try { (void)ask({{"op","connect"},{"definition","other"}}); throw std::logic_error("configuration mismatch accepted"); }
        catch (const std::runtime_error& error) { if (std::string(error.what()).find("differs")==std::string::npos) throw; }
        const auto first=ask({{"op","connect"},{"definition","fixture"}}).at("client");
        const auto terminal = management::connect_resident(data,"fixture").client;
        const auto opening = terminal_reply(data,terminal,{{"id",1},{"action","open"},{"interactive",false}});
        const Json message{{"type","dialogue.submit"},{"version","1"},{"request","a"},{"command","1"},{"text","材料"}};
        if (ask({{"op","request"},{"client",first},{"body",message}}).at("receipt").at("state")!="queued" ||
            !started.try_acquire_for(3s)) throw std::runtime_error("real turn not started");
        (void)ask({{"op","disconnect"},{"client",first}});
        release.release();

        const auto binding = management::connect_resident(data,"fixture");
        const auto second = binding.client;
        const auto initial = binding.watch->observe();
        if (!initial.pid || initial.error || initial.requested || initial.exit_code || binding.instance!=instance)
            throw std::runtime_error("initial peer witness differs");
        const Json read{{"type","dialogue.read"},{"version","1"},{"request","b"},{"command",""},{"revision",""}};
        wait_until([&] {
            const auto state=ask({{"op","request"},{"client",second},{"body",read}});
            const auto& turns=state.at("turns");
            return turns.size()==1 && turns.at(0).at("answer")=="回复：材料" && turns.at(0).at("exit")=="succeeded" &&
                state.at("latestCommand")=="0";
        });
        const auto output = terminal_reply(data,terminal,{{"id",2},{"action","read"},{"after",opening.at("cursor")}});
        if (output.at("out")!="回复：材料\n" || output.at("err")!="") throw std::runtime_error("terminal did not see the desktop result");
        const Json status_line{{"id",3},{"action","line"},{"text","/status"}};
        const auto status_reply = terminal_reply(data,terminal,status_line);
        if (status_reply.at("out").get<std::string>().find("Question=succeeded")==std::string::npos ||
            terminal_reply(data,terminal,status_line)!=status_reply) throw std::runtime_error("terminal lost original status or retry");
        try {
            (void)terminal_reply(data,terminal,{{"id",3},{"action","line"},{"text","/clear"}});
            throw std::logic_error("conflicting terminal retry accepted");
        } catch (const std::runtime_error& error) {
            if (std::string(error.what()).find("conflict")==std::string::npos) throw;
        }
        (void)ask({{"op","disconnect"},{"client",terminal}});
        auto slow = message;
        slow["text"] = "synthetic-private-chat";
        (void)ask({{"op","request"},{"client",second},{"body",slow}});
        if (!started.try_acquire_for(2s)) throw std::runtime_error("second model did not start");
        Json turn;
        wait_until([&] {
            const auto state = ask({{"op","request"},{"client",second},{"body",read}});
            if (state.at("turns").size()!=2) return false;
            turn = state.at("turns").back();
            return turn.at("exit")=="pending";
        });
        const auto accepted = ask({{"op","stop"},{"instance",instance}});
        const auto stopping = binding.watch->observe();
        if (!stopping.requested || stopping.exit_code || stopping.error || stopping.pid!=initial.pid)
            throw std::runtime_error("exit intent was confused with process exit");
        if (ask({{"op","stop"},{"instance",instance}})!=accepted) throw std::runtime_error("repeated stop changed identity");
        try { (void)ask({{"op","status"},{"instance","previous-instance"}}); throw std::logic_error("stale instance progress accepted"); }
        catch (const std::runtime_error& error) { if (std::string(error.what()).find("instance differs")==std::string::npos) throw; }
        wait_until([&] {
            const auto status = ask({{"op","status"},{"instance",instance}});
            const auto& progress = status.at("progress");
            return status.at("operation")==accepted.at("operation") && progress.at("stage")=="conversation" &&
                progress.at("session")==turn.at("session") && progress.at("turn")==turn.at("turn") && progress.at("elapsedMs")>=80;
        });
        if (tray_starts!=1 || tray_releases!=0) throw std::runtime_error("tray lost while original work was draining");
        release.release();
        if (server.get()!=0 || tray_releases!=1) throw std::runtime_error("resident failed to release its tray on exit");
        if (!binding.watch->observe().requested) throw std::runtime_error("intent disappeared when the resident released its event");
        verify_stop_log(data,instance,turn);
    } catch (...) {
        if (server.valid() && !instance.empty()) {
            try { (void)ask({{"op","stop"},{"instance",instance}}); } catch (...) {}
        }
        throw;
    }
}
}

// 用固定模型、独立目录及真实 Windows 管道运行常驻本体验收。
int main(int argc, char** argv) {
    try {
        if (argc!=2) return 2;
        reconnect(argv[1]);
        headless(argv[1],false);
        headless(argv[1],true);
        poll_errors(argv[1]);
        lost_stop_reply(argv[1]);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
