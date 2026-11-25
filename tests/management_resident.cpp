#include "management/resident.hpp"
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

// 让独立观察者先补读原委托，再在下一回合仍在工作时显式退出。
// 步骤：启动隔离宿主 → 接纳后断开与重连 → 在实际调用退出前核对收尾 → 按原实例复查持久回执。
void reconnect(const std::filesystem::path& root) {
    const auto data = root/"user", plugins = root/"plugins";
    std::filesystem::create_directories(plugins);
    std::binary_semaphore started(0), release(0);
    auto server = std::async(std::launch::async,[&] {
        app::SoftwareLog software(data,app::SoftwareRole::host);
        const int code = management::serve_resident(data,"fixture",[&] {
            return std::make_unique<app::Application>(model::Config{},plugins,data,model::Backend{
                [&](const auto&,const auto& input) {
                    started.release();
                    if (!release.try_acquire_for(5s)) throw std::runtime_error("gate timed out");
                    return model::Response{{model::Role::assistant,"回复："+input.messages.back().content}};
                }});
        },{},&software);
        return software.exiting(code);
    });
    const auto ask = [&](Json request) { return management::resident_request(data,std::move(request)); };
    std::string instance;
    try {
        wait_until([&] { const auto status=ask({{"op","status"}}); instance=status.at("instance"); return status.at("state")=="running"; });
        try { (void)ask({{"op","connect"},{"definition","other"}}); throw std::logic_error("configuration mismatch accepted"); }
        catch (const std::runtime_error& error) { if (std::string(error.what()).find("differs")==std::string::npos) throw; }
        const auto first=ask({{"op","connect"},{"definition","fixture"}}).at("client");
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
        release.release();
        if (server.get()!=0) throw std::runtime_error("resident failed to exit");
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
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
