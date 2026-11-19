#include "application.hpp"
#include "application_commands.hpp"
#include <atomic>
#include <iostream>
#include <sstream>
#include <thread>

namespace {
// 在计划板混入另一条任务、吞掉缺口或读取产生动作时结束验收。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
}

// 在正式应用中反复接纳固定查看，核对两个独立保留窗口和只读行为。
// 步骤：查看空会话 → 推进到实际裁剪 → 核对历史任务和消息游标 → 关闭后再读最后记录。
int wmain(int argc, wchar_t** argv) {
    using namespace dfy;
    try {
        check(argc == 3, "package and workspace required");
        const std::filesystem::path root = argv[2];
        std::atomic_uint calls{0};
        app::Application host({}, root / "packages", root / "user", model::Backend{
            [&](const auto&, const auto&) { ++calls; return model::Response{{model::Role::assistant, "只有文字回执"}}; }});
        host.plugins().install(argv[1]);
        check(host.plugins().load("mc.companion") == 0, "load failed");
        const auto session = host.open_companion("mc.companion");
        const auto empty = host.companion_board(session);
        check(empty && empty->tasks.empty() && !empty->selected && !host.companion_board(session + 1), "wrong initial board");
        host.listen_companion(session, app::CompanionMode::scenario);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (host.companion_messages(session, 0).latest < 80 && std::chrono::steady_clock::now() < deadline) {
            host.advance_plugins();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const auto board = host.companion_board(session).value();
        check(calls == 40 && board.messages.latest == 80 && board.messages.gap && board.tasks.size() == 9 &&
            board.selected && !board.historical && board.selected->visit.order == 40, "retained snapshot or gap incorrect");
        check(board.tasks.front() == 63 && board.tasks.back() == 79 && board.discarded_through == 61, "task history boundary");
        const auto past = host.companion_board(session, 63, 62).value();
        check(past.historical && past.selected && past.selected->visit.text == "固定查看 32" && !past.selected->collection &&
            !past.messages.gap && past.messages.messages.size() == 2, "mixed historical task");
        for (const auto& msg : past.messages.messages) check(msg.task == 63 && msg.address == past.selected->visit.address, "foreign message");
        check(!host.companion_board(session, 61)->selected && !host.companion_board(session, 91)->selected, "invented task");
        const auto repeated = host.companion_board(session, 63, 80).value();
        check(repeated.messages.messages.empty() && repeated.selected->visit.task == 63 && calls == 40, "query advanced work");
        std::ostringstream out;
        app::application_command(host, "/companion board " + std::to_string(session) + " 61", out, std::cerr);
        check(out.str().find("保留窗口") != std::string::npos, "eviction was hidden");

        host.stop_companion(session);
        check(!host.companion_board(session)->binding_current && host.companion_board(session)->selected, "closed history lost");
        host.conversation().close();
        while (host.close_plugins() == plugins::ShutdownResult::pending && std::chrono::steady_clock::now() < deadline) host.advance_plugins();
        check(host.close_plugins() == plugins::ShutdownResult::closed, "host did not close");
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
