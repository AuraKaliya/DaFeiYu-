#include "companion_host.hpp"
#include "application_commands.hpp"
#include <dafeiyu/line_input.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

namespace dfy::app {
// 启动独立角色，通过正式应用读取原话、推进模型与插件工作并保存同局对话。
// 步骤：安装并加载本地包 → 等待玩家聊天水位 → 推进及发布进展 → 停止并回收。
void run_companion_host(Application& host, const std::filesystem::path& package, const std::filesystem::path& root, CompanionMode mode) {
    if (const auto path = host.trace_file(); !path.empty()) std::cout << "DFY_TRACE " << path.generic_string() << std::endl;
    host.plugins().install(package);
    if (host.plugins().load("mc.companion") != 0) throw std::runtime_error("load MC plugin failed");
    const auto id = host.open_companion("mc.companion");
    host.listen_companion(id, mode);
    bool listening = false;
    std::uint64_t reported_event = 0, presented = 0;
    std::unique_ptr<platform::LineInput> input;
    if (mode == CompanionMode::partner) {
        try { input = std::make_unique<platform::LineInput>(); }
        catch (const std::exception&) { }
    }
    std::string previous;
    auto next = std::chrono::steady_clock::now();
    while (!host.companion_status()->closed) {
        if (host.advance_plugins() == runtime::RunResult::failed) throw std::runtime_error("game host step failed");
        host.conversation().advance();
        if (const auto question = host.companion_confirmation(); question && question->channel == "panel" &&
            question->decision.empty() && presented != question->id) {
            presented = question->id;
            std::cout << question->question << "\n/companion confirm " << question->id << ' ' << question->revision << " approve|deny" << std::endl;
        }
        if (input) {
            const auto line = input->poll();
            if (line.state == platform::InputState::line && line.line.starts_with("/companion confirm ")) {
                try { application_command(host,line.line,std::cout,std::cerr); }
                catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
            }
            input->advance();
        }
        if (!listening && host.companion_inbox(id)->ready) {
            listening = true;
            std::cout << "DFY_LISTENING" << std::endl;
        }
        const auto settled = host.companion_status()->settled_event;
        if (reported_event != settled) {
            reported_event = settled;
            if (settled) std::cout << "DFY_EVENT_SETTLED " << settled <<
                (host.companion_inbox(id)->departed == settled ? " left" : "") << std::endl;
        }
        if (std::chrono::steady_clock::now() >= next || host.companion_status()->closed) {
            if (std::filesystem::exists(root / "stop")) host.stop_companion(id);
            std::ostringstream board;
            app::application_command(host, "/companion status", board, std::cerr);
            application_command(host, "/companion board " + std::to_string(id), board, std::cerr);
            if (board.str() != previous) {
                previous = board.str();
                std::ofstream(root / "board.txt", std::ios::binary) << previous;
            }
            next = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    host.conversation().close();
    while (!host.conversation().closed()) {
        host.advance_plugins();
        host.conversation().advance();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    while (host.close_plugins() == plugins::ShutdownResult::pending) host.advance_plugins();
    if (host.close_plugins() != plugins::ShutdownResult::closed) throw std::runtime_error("game host did not exit");
}
}
