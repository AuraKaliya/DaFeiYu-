#include "application.hpp"
#include "application_commands.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

namespace {
using namespace dfy;
// 有界推进正式宿主，等待实际消息、读回或子回合结束。
void until(app::Application& app, const auto& ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!ready() && std::chrono::steady_clock::now() < deadline) {
        app.advance_plugins();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!ready()) throw std::runtime_error("exploration condition timed out");
}
// 首次查看由固定模型选择原工具；路线查询不应占用另一次模型请求。
model::Response fixed(const model::Request& request) {
    const auto& message = request.messages.back();
    if (message.role == model::Role::tool) return {{model::Role::assistant, "查看已结束"}};
    if (!message.content.starts_with("去山脚看看") || request.tools.size() != 1) throw std::runtime_error("unexpected request");
    return {{model::Role::assistant, "", {{"inspect", request.tools.front().name, message.content.substr(message.content.rfind('\n') + 1)}}},
        model::FinishReason::tool_calls};
}
// 检验持续探索、核实后追问及断线三种确定行为。
// 步骤：从真实插件接纳固定委托 → 核对主侧决定 → 分开验证子答复和新观察 → 查询终端依据。
void verify(const std::filesystem::path& pkg, const std::filesystem::path& root, std::wstring_view mode) {
    app::Application app({}, root / "packages", root / "user", model::Backend{
        [](const auto&, const auto& request) { return fixed(request); }});
    app.plugins().install(pkg);
    if (app.plugins().load("mc.companion")) throw std::runtime_error("plugin load failed");
    const auto id = app.open_companion("mc.companion");
    app.listen_companion(id, dfy::app::CompanionMode::scenario);
    const auto review = [&] { return app.companion_review(id); };
    until(app, [&] { return review().has_value(); });
    const auto first = *review();
    if (mode == L"exploring") {
        if (first.stage != app::ReviewStage::exploring || first.inquiry || first.coverage != 4) throw std::runtime_error("unexpected intervention");
        std::cout << "finish" << std::endl;
    } else {
        if (first.stage != app::ReviewStage::rechecking || first.inquiry) throw std::runtime_error("missing independent recheck");
        const auto pending = app.refresh_companion(id);
        until(app, [&] { auto s = app.companion_state(id); return s->request == pending && !s->pending; });
        if (review()->inquiry) throw std::runtime_error("cached sample triggered inquiry");
        if (mode == L"disconnect") {
            std::cout << "disconnect" << std::endl;
            until(app, [&] { return !app.companion_address(); });
            if (review()->stage != app::ReviewStage::interrupted || review()->inquiry || review()->recovery) throw std::runtime_error("disconnect counted as evidence");
        } else if (mode == L"recheck") {
            std::cout << "recover" << std::endl;
            until(app, [&] { return review()->stage == app::ReviewStage::exploring; });
            if (review()->inquiry || review()->coverage != 2) throw std::runtime_error("ignored fresh progress");
            std::cout << "finish" << std::endl;
        } else {
            std::cout << "confirm" << std::endl;
            until(app, [&] { return review()->inquiry != 0; });
            until(app, [&] { return review()->answer != 0; });
            if (review()->recovery) throw std::runtime_error("premature recovery");
            if (mode == L"gap") {
                std::cout << "new-window" << std::endl;
                until(app, [&] { return review()->stage == app::ReviewStage::interrupted; });
                if (review()->recovery) throw std::runtime_error("gap credited as recovery");
            } else {
                std::cout << "recover" << std::endl;
                until(app, [&] { return review()->recovery != 0; });
            }
            std::cout << "finish" << std::endl;
            const auto last = *review();
            unsigned inquiries = 0;
            bool paired = false;
            for (const auto& msg : app.companion_messages(id, 0).messages) {
                if (msg.kind == app::CompanionMessageKind::inquiry) { ++inquiries; if (msg.reply_to != first.report) throw std::runtime_error("wrong evidence"); }
                if (msg.revision == last.answer) paired = msg.reply_to == last.inquiry && msg.task == first.task;
            }
            if (!paired || inquiries != 1 || last.reply.find("西侧") == std::string::npos)
                throw std::runtime_error("missing linked answer or repeated inquiry");
        }
    }
    std::ostringstream output;
    app::application_command(app, "/companion status", output, std::cerr);
    if (output.str().find(review()->basis) == std::string::npos) throw std::runtime_error("review missing from trusted status");
    const auto board = app.companion_board(id, first.task);
    if (!board || !board->selected || !board->selected->review || board->selected->review->report != review()->report ||
        board->selected->review->verification != review()->verification || board->binding_current == (mode == L"disconnect"))
        throw std::runtime_error("board lost original intervention or binding");
    std::ofstream transcript(root / "review.txt", std::ios::binary);
    transcript.exceptions(std::ios::badbit | std::ios::failbit);
    transcript << output.str() << "\n主子消息记录\n";
    for (const auto& message : app.companion_messages(id, 0).messages)
        transcript << '#' << message.revision << "  原消息 #" << message.reply_to << "  " << message.text << '\n';
    transcript.close();
    app.stop_companion(id);
    until(app, [&] { return app.companion_status()->closed; });
    app.conversation().close();
    if (app.close_plugins() != plugins::ShutdownResult::closed) throw std::runtime_error("cleanup failed");
}
}
// 启动使用真实 DLL 和固定轨迹的受阻分析验收。
int wmain(int argc, wchar_t** argv) {
    try { if (argc != 4) return 1; verify(argv[1], argv[2], argv[3]); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
