#include "management/host.hpp"
#include "management/protocol.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <semaphore>

namespace {
using namespace std::chrono_literals;
using namespace dfy;

// 有界等待真实业务线程完成检查所需的推进。
template<class Predicate>
void until(Predicate ready, const char* message) {
    const auto limit = std::chrono::steady_clock::now()+3s;
    while (!ready()) {
        if (std::chrono::steady_clock::now() >= limit) throw std::runtime_error(message);
        std::this_thread::sleep_for(5ms);
    }
}

// 在无窗口时完成用户回合，新观察者能取得原回合、正文及实际退出。
// 步骤：打开空插件宿主 → 提交后释放观察快照 → 放行固定答复 → 验证重读、幂等及显式关闭。
void background_turn(const std::filesystem::path& root) {
    std::filesystem::create_directories(root/"plugins");
    std::binary_semaphore entered(0), release(0);
    std::atomic<int> calls = 0;
    management::Host host([&] {
        return std::make_unique<app::Application>(model::Config{},root/"plugins",root/"user",model::Backend{
            [&](const auto&, const auto& input) {
                ++calls;
                entered.release();
                if (!release.try_acquire_for(4s)) throw std::runtime_error("fixture release timed out");
                return model::Response{{model::Role::assistant,"已收到："+input.messages.back().content}};
            }});
    });
    until([&] { return host.dialogue().accepting; },"host did not start");
    const management::DialogueCommand command{1,management::DialogueAction::send,{},"在后台处理",0};
    if (host.submit_dialogue(command).state != management::DialogueCommandState::queued || !entered.try_acquire_for(3s))
        throw std::runtime_error("turn did not enter the model fixture");
    until([&] {
        const auto first = host.dialogue(1);
        return first.display && first.display->turns.size() == 1 && first.receipt &&
            first.receipt->state == management::DialogueCommandState::accepted;
    },"original receipt missing");

    release.release();
    until([&] { return host.dialogue().accepting; },"background turn did not exit");
    const auto later = host.dialogue(1);
    const auto& turn = later.display->turns.at(0);
    if (!later.receipt || turn.run != later.receipt->run || turn.answer != "已收到：在后台处理" ||
        turn.result != runtime::OperationState::succeeded || turn.exit != runtime::OperationState::succeeded ||
        host.submit_dialogue(command).run != turn.run || calls != 1)
        throw std::runtime_error("observer did not recover the original completed turn");

    const auto room = management::dispatch(&host,{{"version","1"},{"request","position"},{"type","room.read"}});
    const auto view = management::dispatch(&host,{{"version","1"},{"request","chat"},{"type","dialogue.read"},{"revision",""},{"command","1"}});
    const auto status = management::dispatch(&host,{{"version","1"},{"request","status"},{"type","state.read"},{"topic","run"}});
    if (room.at("room").at("placeName") != "书桌旁" || view.at("turns").at(0).at("answer") != turn.answer ||
        status.at("run").at("exit") != "succeeded" || status.dump().find(turn.answer) != std::string::npos)
        throw std::runtime_error("management protocol exposed the wrong projection");

    host.close();
    until([&] { return host.exited(); },"explicit shutdown did not finish");
    if (host.failed() || host.dialogue().accepting) throw std::runtime_error("host did not close cleanly");
}
}

// 在 CTest 分配的目录运行无窗口业务生命周期回归。
int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        background_turn(argv[1]);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
