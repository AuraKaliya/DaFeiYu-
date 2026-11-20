#include "application.hpp"
#include <algorithm>
#include <atomic>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>

// 从正式应用验证主 Agent 选择渠道及游戏内本地确认，执行仍由原插件裁定。
// 步骤：注入固定角色策略 → 推进真实收发及主回合 → 从原渠道答复 → 核对真实候选结算并释放。
int wmain(int argc, wchar_t** argv) {
    using namespace dfy;
    using namespace model;
    using Json = nlohmann::json;
    try {
        if (argc != 4) throw std::runtime_error("package, test directory and route are required");
        const std::filesystem::path root = argv[2];
        const bool panel = std::wstring_view(argv[3]) == L"partner-route-panel";
        const bool local = std::wstring_view(argv[3]) == L"partner-route-local";
        std::atomic_uint main_calls = 0;
        // 分别提供子 Agent 的原候选和主 Agent 的固定渠道选择。
        app::Application host({},root/"packages",root/"user",Backend{[&](const auto&,const Request& input) -> Response {
            const auto& last = input.messages.back();
            if (input.tools.size() == 2) {
                ++main_calls;
                if (last.role == Role::tool) return {{Role::assistant,"routing finished"}};
                const auto mail = Json::parse(last.content);
                if (mail.at("arguments").at("count") != 2 || mail.at("category") != "mc.confirmation.request")
                    throw std::runtime_error("main did not receive original candidate");
                return {{Role::assistant,"",{{"present",panel ? "present_confirmation_on_panel" : "present_confirmation_in_game","{}"}}},FinishReason::tool_calls};
            }
            if (last.role == Role::tool) return {{Role::assistant,"collection finished"}};
            const auto event = Json::parse(last.content).at("events").back();
            if (event.at("sequence") != 2) return {{Role::assistant,"no game action"}};
            const auto tool = std::ranges::find_if(input.tools,[](const auto& tool) { return tool.name.ends_with("_collect_confirmed"); });
            if (tool == input.tools.end()) throw std::runtime_error("confirmed collection unavailable");
            const auto args = Json{{"block","dirt"},{"count",2},{"route",local ? "game" : "main"}}.dump();
            return {{Role::assistant,"",{{"collect",tool->name,args}}},FinishReason::tool_calls};
        }});
        host.plugins().install(argv[1]);
        if (host.plugins().load("mc.companion")) throw std::runtime_error("could not load MC package");
        const auto id = host.open_companion("mc.companion");
        host.listen_companion(id);
        bool requested_stop = false;
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline) {
            if (host.advance_plugins() == runtime::RunResult::failed) throw std::runtime_error("confirmation work failed");
            host.conversation().advance();
            const auto state = host.companion_status();
            const auto question = host.companion_confirmation();
            if (panel && question && question->channel == "panel" && !question->answered) {
                host.decide_companion_confirmation(question->id,question->revision,true);
                host.decide_companion_confirmation(question->id,question->revision,false);
            }
            if (!requested_stop && state->settled_event == (panel ? 2u : 3u) &&
                (local || (question && question->decision == "approved" && question->source == (panel ? "main" : "game") &&
                    (panel || question->presentation == "sent")))) {
                host.stop_companion(id);
                requested_stop = true;
            }
            if (requested_stop && state->closed) {
                if (main_calls != (local ? 0u : 2u) || !state->control || state->control->phase != app::ControlPhase::released)
                    throw std::runtime_error("channel, model routing or original game exit not verified");
                host.conversation().close();
                if (host.close_plugins() != plugins::ShutdownResult::closed) throw std::runtime_error("game plugin failed to close");
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        throw std::runtime_error("confirmation did not reach its real exit");
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
