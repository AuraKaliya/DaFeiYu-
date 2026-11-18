#include "application.hpp"
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace dfy;
// 在读口串线或观察新旧边界失效时报告失败。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 推进真实应用读取，直到带有本次请求身份的工作实际退出。
app::CompanionState refresh(app::Application& app, runtime::DomainId id) {
    const auto receipt = app.refresh_companion(id);
    check(app.refresh_companion(id) == receipt && app.companion_state(id)->pending, "inflight state query duplicated");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (app.companion_state(id)->pending && std::chrono::steady_clock::now() < deadline) {
        app.advance_plugins();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto state = *app.companion_state(id);
    check(state.request == receipt && !state.pending, "missing readback");
    return state;
}
}

// 在正式应用中取得可信世界来源，并区分有值、撤销与最后一次已知记录。
// 步骤：启动真实角色插件 → 两次原生读回 → 核对零模型调用和停止收尾。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 3, "expected package and test workspace");
        const std::filesystem::path root = argv[2];
        std::atomic_uint calls{0};
        app::Application application({}, root / "packages", root / "user", model::Backend{
            [&](const auto&, const auto&) -> model::Response { ++calls; throw std::runtime_error("unexpected model"); }});
        auto& manager = application.plugins();
        manager.install(argv[1]);
        check(manager.load("mc.companion") == 0, "load failed");
        const auto id = application.open_companion("mc.companion");
        check(application.companion_state(id)->link == app::CompanionLink::unknown, "implicit world read");
        const auto live = refresh(application, id);
        check(live.link == app::CompanionLink::available && live.last && !live.last->actor.empty(), "missing live state");
        const auto lost = refresh(application, id);
        check(lost.link == app::CompanionLink::unbound && lost.last && lost.last->generation == live.last->generation &&
            lost.last->received == live.last->received && lost.request != live.request, "old state presented as new");
        check(!application.companion_state(id + 1) && calls == 0, "wrong session or unexpected model");
        application.stop_companion(id);
        application.conversation().close();
        check(application.close_plugins() == plugins::ShutdownResult::closed, "cleanup failed");
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
