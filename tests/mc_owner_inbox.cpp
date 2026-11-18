#include "application.hpp"
#include <iostream>
#include <thread>

namespace {
// 在真实游戏等待期间推进共享宿主，所有模型输出由调用方显式替代。
void until(dfy::app::Application& app, const auto& ready) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!ready() && std::chrono::steady_clock::now() < end) {
        app.advance_plugins();
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    if (!ready()) throw std::runtime_error("player inbox timeout");
}
}
// 从实际启动的游戏读出旧水位，在启用后接收新玩家原话。
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 3) return 1;
        const std::filesystem::path root = argv[2];
        dfy::app::Application app({}, root / "packages", root / "user", dfy::model::Backend{
            [](const auto&, const auto&) -> dfy::model::Response { throw std::runtime_error("unexpected model"); }});
        app.plugins().install(argv[1]);
        if (app.plugins().load("mc.companion")) return 1;
        const auto id = app.open_companion("mc.companion");
        app.listen_companion(id);
        until(app, [&] { return app.companion_inbox(id)->ready; });
        const auto baseline = *app.companion_inbox(id);
        if (baseline.cursor != 1 || baseline.owner.empty() || !app.companion_messages(id, 0).messages.empty()) return 1;
        std::cout << "DFY_LISTENING" << std::endl;
        until(app, [&] { return app.companion_inbox(id)->cursor == 2; });
        auto live = *app.companion_inbox(id);
        if (live.owner != baseline.owner || live.gap) return 1;
        app.stop_companion(id);
        until(app, [&] { return app.companion_status()->closed; });
        app.conversation().close();
        return app.close_plugins() == dfy::plugins::ShutdownResult::closed ? 0 : 1;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
