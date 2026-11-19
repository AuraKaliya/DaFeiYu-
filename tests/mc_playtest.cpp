#include "application_commands.hpp"
#include "application.hpp"
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>

namespace {
// 体验服用已授权的单一查看参数调用正式工具，游戏事实只取自插件回执。
dfy::model::Response sample_turn(const dfy::model::Request& request) {
    using namespace dfy::model;
    const auto& last = request.messages.back();
    if (last.role == Role::tool || request.tools.size() != 1) return {{Role::assistant, "本轮查看已结束。"}};
    const auto newline = last.content.rfind('\n');
    if (newline == std::string::npos) return {{Role::assistant, "我在这里。"}};
    const auto args = nlohmann::json::parse(last.content.substr(newline + 1), nullptr, false);
    if (!args.is_object() || !args.contains("order") || !args.contains("place")) return {{Role::assistant, "我在这里。"}};
    return {{Role::assistant, "", {{"visit", request.tools.front().name, args.dump()}}}, FinishReason::tool_calls};
}
}

// 保持真人体验的正式宿主运行，向本地日志发布进展和计划板并完整收尾。
// 步骤：绑定原生包和独立数据目录 → 推进玩家委托与回执 → 按退出请求回收插件。
int wmain(int argc, wchar_t** argv) {
    using namespace dfy;
    try {
        if (argc != 3) throw std::invalid_argument("package and playtest directory required");
        const std::filesystem::path root = argv[2];
        app::Application host({}, root / "packages", root / "user", model::Backend{
            [](const auto&, const auto& request) { return sample_turn(request); }});
        host.plugins().install(argv[1]);
        if (host.plugins().load("mc.companion") != 0) throw std::runtime_error("load MC plugin failed");
        const auto id = host.open_companion("mc.companion");
        host.listen_companion(id);
        bool listening = false;
        std::string previous;
        auto next = std::chrono::steady_clock::now();
        while (!host.companion_status()->closed) {
            host.advance_plugins();
            if (!listening && host.companion_inbox(id)->ready) {
                listening = true;
                std::cout << "DFY_LISTENING" << std::endl;
            }
            if (std::chrono::steady_clock::now() >= next || host.companion_status()->closed) {
                if (std::filesystem::exists(root / "stop")) host.stop_companion(id);
                std::ostringstream board;
                app::application_command(host, "/companion status", board, std::cerr);
                app::application_command(host, "/companion board " + std::to_string(id), board, std::cerr);
                if (board.str() != previous) {
                    previous = board.str();
                    std::ofstream(root / "board.txt", std::ios::binary) << previous;
                }
                next = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        host.conversation().close();
        while (host.close_plugins() == plugins::ShutdownResult::pending) host.advance_plugins();
        if (host.close_plugins() != plugins::ShutdownResult::closed) throw std::runtime_error("game host did not exit");
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
