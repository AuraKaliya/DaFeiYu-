#include "application.hpp"
#include <iostream>
#include <thread>

namespace {
using namespace dfy;
using Kind = app::CompanionMessageKind;
// 检查真实会话是否按原委托及问题一次性处理。
void check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
// 等待宿主推进到当前测试所需的消息或观察。
void until(app::Application& application, const auto& ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (!ready() && std::chrono::steady_clock::now() < deadline) {
        application.advance_plugins();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(ready(), "companion exchange timeout");
}
}
// 通过原生状态读取绑定世界并用固定模型完成两次定向对话。
// 步骤：取得真实插件读回 → 委托和重复命令 → 针对原报告追问 → 撤销并拒绝旧世界。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 3, "package and fixture required");
        const std::filesystem::path root = argv[2];
        std::atomic_uint calls{0};
        app::Application application({}, root / "packages", root / "user", model::Backend{
            [&](const auto&, const model::Request& request) {
                ++calls;
                const auto& prompt = request.messages.back().content;
                if (prompt == "请查看") return model::Response{{model::Role::assistant, "已到山脚"}};
                if (prompt == "哪一侧") return model::Response{{model::Role::assistant, "西侧"}};
                throw std::runtime_error("unexpected delegated request");
            }});
        application.plugins().install(argv[1]);
        check(application.plugins().load("mc.companion") == 0, "load failed");
        const auto session = application.open_companion("mc.companion");
        application.refresh_companion(session);
        until(application, [&] { return application.companion_address().has_value(); });
        const auto address = *application.companion_address();
        const auto task = application.send_companion(address, Kind::delegation, 1, 0, "请查看");
        check(application.send_companion(address, Kind::delegation, 1, 0, "请查看") == task, "duplicate queued message");
        until(application, [&] { return application.companion_messages(session, task).latest == 2; });
        const auto report = application.companion_messages(session, task).messages.front();
        check(report.kind == Kind::report && report.task == task && report.text == "已到山脚" && report.reply_to == task, "wrong report");
        const auto question = application.send_companion(address, Kind::inquiry, 2, report.revision, "哪一侧");
        until(application, [&] { return application.companion_messages(session, question).latest == 4; });
        const auto answer = application.companion_messages(session, question).messages.front();
        check(answer.kind == Kind::answer && answer.text == "西侧" && answer.task == task && answer.reply_to == question, "wrong answer");
        check(application.send_companion(address, Kind::inquiry, 2, report.revision, "哪一侧") == question && calls == 2, "duplicate execution");

        application.refresh_companion(session);
        until(application, [&] { return !application.companion_address(); });
        bool rejected = false;
        try { application.send_companion(address, Kind::delegation, 3, 0, "迟到"); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected && application.companion_messages(session, 0).latest == 4 && calls == 2, "revoked world revived");
        application.stop_companion(session);
        application.conversation().close();
        check(application.close_plugins() == plugins::ShutdownResult::closed, "close failed");
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
