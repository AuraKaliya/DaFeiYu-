#include "management/resident.hpp"
#include <nlohmann/json.hpp>
#include <future>
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

// 通过本地连接验证所有观察者离开期间，原委托仍能在同一本体上完成。
// 步骤：启动隔离宿主 → 受控接纳后断开 → 新观察者读回原消息 → 按所属实例关闭并核对退出。
void reconnect(const std::filesystem::path& root) {
    const auto data = root/"user", plugins = root/"plugins";
    std::filesystem::create_directories(plugins);
    std::binary_semaphore started(0), release(0);
    auto server = std::async(std::launch::async,[&] {
        return management::serve_resident(data,"fixture",[&] {
            return std::make_unique<app::Application>(model::Config{},plugins,data,model::Backend{
                [&](const auto&,const auto& input) {
                    started.release();
                    if (!release.try_acquire_for(5s)) throw std::runtime_error("gate timed out");
                    return model::Response{{model::Role::assistant,"回复："+input.messages.back().content}};
                }});
        });
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

        const auto second=ask({{"op","connect"},{"definition","fixture"}}).at("client");
        const Json read{{"type","dialogue.read"},{"version","1"},{"request","b"},{"command",""},{"revision",""}};
        wait_until([&] {
            const auto state=ask({{"op","request"},{"client",second},{"body",read}});
            const auto& turns=state.at("turns");
            return turns.size()==1 && turns.at(0).at("answer")=="回复：材料" && turns.at(0).at("exit")=="succeeded" &&
                state.at("latestCommand")=="0";
        });
        (void)ask({{"op","stop"},{"instance",instance}});
        if (server.get()!=0) throw std::runtime_error("resident failed to exit");
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
