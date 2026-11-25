#include "application.hpp"
#include "application_commands.hpp"
#include <iostream>
#include <stdexcept>
#include <sstream>
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
        const auto recent = application.locate_companion(id, app::LocationPurpose::progress);
        check(recent.basis == app::LocationBasis::recent && !recent.refresh, "recent progress queried again");
        std::ostringstream readable;
        app::application_command(application, "/companion where " + std::to_string(id), readable, std::cerr);
        check(readable.str().find("Recent confirmed progress") != std::string::npos, "terminal missing decision basis");
        const auto later = live.last->received + std::chrono::milliseconds(1500);
        check(app::location_basis(live, app::LocationPurpose::progress, true, later, std::chrono::seconds(1)) == app::LocationBasis::aging &&
            app::location_basis(live, app::LocationPurpose::progress, false, later, std::chrono::seconds(1)) == app::LocationBasis::recent,
            "movement did not change freshness");
        const auto meeting = application.locate_companion(id, app::LocationPurpose::meeting);
        check(meeting.basis == app::LocationBasis::confirm_meeting && meeting.refresh && meeting.last, "meeting reused old coordinates");
        app::application_command(application, "/companion where " + std::to_string(id) + " meet", readable, std::cerr);
        check(readable.str().find(std::to_string(meeting.refresh)) != std::string::npos, "terminal did not reuse pending refresh");
        const auto lost = refresh(application, id);
        check(lost.link == app::CompanionLink::unbound && lost.last && lost.last->generation == live.last->generation &&
            lost.last->received == live.last->received && lost.request != live.request && lost.request == meeting.refresh &&
            app::location_basis(lost, app::LocationPurpose::progress, false, later, std::chrono::seconds(1)) == app::LocationBasis::disconnected,
            "old state presented as new");
        check(!application.companion_state(id + 1) && calls == 0, "wrong session or unexpected model");
        application.stop_companion(id);
        application.conversation().close();
        check(application.close_plugins() == plugins::ShutdownResult::closed, "cleanup failed");
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
