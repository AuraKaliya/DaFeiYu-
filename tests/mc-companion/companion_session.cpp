#include "scenario_request.hpp"
#include "application.hpp"
#include <future>
#include <iostream>
#include <thread>

namespace {
using namespace dfy;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
// 在两个会话的身份或寿命约束失效时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 生成不覆盖旧证据的测试目录。
fs::path workspace(const fs::path& root) {
    fs::create_directories(root);
    for (unsigned index = 1;; ++index) {
        const auto result = root / std::to_string(index);
        if (fs::create_directory(result)) return result;
    }
}
// 确认失效入口会拒绝调用。
void refuses(const auto& action) {
    try { action(); } catch (const std::logic_error&) { return; }
    throw std::runtime_error("stale companion command accepted");
}
// 有界推进共享插件、子会话和主会话。
void until(app::Application& application, const auto& ready) {
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (!ready() && std::chrono::steady_clock::now() < deadline) {
        application.advance_plugins();
        application.conversation().advance();
        std::this_thread::sleep_for(1ms);
    }
    check(ready(), "companion lifecycle timeout");
}
// 在真实包和可控后台响应上验证同一应用的两个独立会话。
// 步骤：拒绝普通包 → 提交子会话等待 → 推进主会话 → 失效并等待原后台退出 → 重开新代次。
void verify(const fs::path& package, const fs::path& ordinary, const fs::path& root) {
    std::promise<void> entered, release;
    auto started = entered.get_future();
    const auto gate = release.get_future().share();
    app::Application application({}, root / "packages", root / "user", model::Backend{
        [&](const auto&, const model::Request& formal) {
            const auto request = dfy::mc_test::scenario_request(formal);
            const auto& prompt = request.messages.back().content;
            if (prompt == "wait here") {
                check(request.tools.size() == 1, "child readonly tool missing");
                entered.set_value();
                gate.wait_for(5s);
            } else if (prompt == "where are you") {
                check(request.tools.empty() && request.messages.size()==2 && request.messages.front().source==model::MessageSource::rules &&
                    request.messages.back().source==model::MessageSource::human, "child context entered primary session");
            }
            return model::Response{{model::Role::assistant, "answer:" + prompt}};
        }});
    auto& manager = application.plugins();
    manager.install(ordinary);
    check(manager.load("example.status") == 0, "ordinary load");
    refuses([&] { application.open_companion("example.status"); });
    check(!application.companion_status(), "ordinary plugin started a child");
    manager.install(package);
    check(manager.load("mc.companion") == 0, "MC load");
    const auto original = application.open_companion("mc.companion");
    check(original != application.conversation().id() && application.open_companion("mc.companion") == original, "child identity");
    const auto run = application.submit_companion(original, "wait here");
    until(application, [&] { return started.wait_for(0ms) == std::future_status::ready; });

    application.conversation().submit("where are you");
    until(application, [&] { return !application.conversation().busy(); });
    check(application.conversation().status()->result == runtime::OperationState::succeeded &&
        application.companion_status()->turn->result == runtime::OperationState::pending, "primary blocked by child");
    check(manager.unload("mc.companion") == plugins::ShutdownResult::closed, "idle native registration retained DLL");
    refuses([&] { application.submit_companion(original, "late action"); });
    application.advance_plugins();
    check(application.companion_status()->stopping && !application.companion_status()->closed, "early child exit");
    check(manager.load("mc.companion") == 0, "MC reload");
    refuses([&] { application.open_companion("mc.companion"); });
    release.set_value();
    until(application, [&] { return application.companion_status()->closed; });
    const auto cancelled = application.companion_result({original, run.turn});
    check(cancelled && cancelled->status.result == runtime::OperationState::cancelled, "late model revived child");

    const auto replacement = application.open_companion("mc.companion");
    check(replacement != original && !application.companion_result({original, run.turn}), "stale results leaked");
    refuses([&] { application.stop_companion(original); });
    auto second = application.submit_companion(replacement, "hello");
    until(application, [&] { return application.companion_status()->turn->exit != runtime::OperationState::pending; });
    check(application.companion_result({replacement, second.turn})->answer == "answer:hello", "new child result");
    application.conversation().close();
    check(application.close_plugins() == plugins::ShutdownResult::closed && application.companion_status()->closed, "shutdown order");
}
}

// 运行共享应用与真实原生包的陪伴会话测试。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 4, "two packages and workspace required");
        verify(argv[1], argv[2], workspace(argv[3]));
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
