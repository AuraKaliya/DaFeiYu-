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
        const bool manual = std::wstring_view(argv[3]) == L"partner-route-manual";
        const bool first = std::wstring_view(argv[3]) == L"partner-route-first";
        const bool panel = manual || std::wstring_view(argv[3]) == L"partner-route-panel";
        const bool local = std::wstring_view(argv[3]) == L"partner-route-local";
        std::atomic_uint main_calls = 0;
        // 分别提供子 Agent 的原候选和主 Agent 的固定渠道选择。
        app::Application host({},root/"packages",root/"user",Backend{[&](const auto&,const Request& input) -> Response {
            const auto& last = input.messages.back();
            const auto routed = std::ranges::find_if(input.tools,[](const auto& tool) {
                return tool.name == "ask" || tool.name == "mail_reply";
            });
            if (routed != input.tools.end()) {
                ++main_calls;
                if (last.role == Role::tool) return {{Role::assistant,"routing finished"}};
                const auto envelope = Json::parse(last.content);
                const auto mail = Json::parse(envelope.at("content").get<std::string>());
                if (routed->name == "mail_reply") {
                    if (envelope.at("category") != "interaction.ask.completed" || mail.at("answer") != "approve")
                        throw std::runtime_error("main did not receive original answer");
                    return {{Role::assistant,"",{{"reply","mail_reply","{}"}}},FinishReason::tool_calls};
                }
                const auto& context = mail.at("context");
                if (context.at("skill") != "collect" || context.at("arguments").at("count") != 2 ||
                    context.at("arguments").contains("approval") || envelope.at("category") != "mc.partner.question")
                    throw std::runtime_error("main did not receive original candidate");
                return {{Role::assistant,"",{{"present","ask",panel ? R"({"channel":"terminal"})" : R"({"channel":"game"})"}}},
                    FinishReason::tool_calls};
            }
            if (input.tools.empty()) return {{Role::assistant,"continue"}};
            if (last.role == Role::tool) return {{Role::assistant,"collection finished"}};
            const auto body = Json::parse(last.content);
            const auto expanded = body.at("trigger") == "skills.expanded";
            const auto& event = body.at(expanded ? "recentEvents" : "events").back();
            if (event.at("sequence") != 2) return {{Role::assistant,"no game action"}};
            if (!expanded) {
                const auto directory = std::ranges::find_if(input.tools,[](const auto& tool) { return tool.name.ends_with("_skills"); });
                if (directory == input.tools.end()) throw std::runtime_error("skill directory unavailable");
                return {{Role::assistant,"",{{"expand",directory->name,R"({"categories":["dig_break"]})"}}},FinishReason::tool_calls};
            }
            const auto tool = std::ranges::find_if(input.tools,[](const auto& tool) { return tool.name.ends_with("_collect"); });
            if (tool == input.tools.end()) throw std::runtime_error("confirmed collection unavailable");
            const auto args = Json{{"block","dirt"},{"count",2},{"approval",local ? "game" : "main"}}.dump();
            return {{Role::assistant,"",{{"collect",tool->name,args}}},FinishReason::tool_calls};
        }});
        host.plugins().install(argv[1]);
        host.wake_for_plugin_mail(!manual);
        if (host.plugins().load("mc.companion")) throw std::runtime_error("could not load MC package");
        const auto id = host.open_companion("mc.companion");
        host.listen_companion(id);
        bool requested_stop = false;
        unsigned manual_wait = 0, reviews = 0;
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline) {
            if (host.advance_plugins() == runtime::RunResult::failed) throw std::runtime_error("confirmation work failed");
            host.conversation().advance();
            const auto state = host.companion_status();
            if (manual && !reviews) {
                if (main_calls != 0 || host.conversation().busy()) throw std::runtime_error("passive confirmation woke the Agent");
                if (++manual_wait > 5 && host.review_mail() == app::MailboxIntakeResult::submitted) ++reviews;
            }
            if (panel) if (const auto question = host.console_ask()) {
                if (host.answer_console_ask(question->first,"approve") ||
                    !host.answer_console_ask(question->first,{}) ||
                    !host.answer_console_ask(question->first,"approve") ||
                    !host.answer_console_ask(question->first,"approve") ||
                    host.answer_console_ask(question->first,"deny"))
                    throw std::runtime_error("terminal answer was accepted before display or overwritten");
            }
            if (manual && reviews == 1 && main_calls == 2 && host.review_mail() == app::MailboxIntakeResult::submitted) ++reviews;
            if (!requested_stop && state->settled_event == (panel ? 2u : first ? 4u : 3u) &&
                (local || (main_calls == 4 && !host.conversation().busy()))) {
                host.stop_companion(id);
                requested_stop = true;
            }
            if (requested_stop && state->closed) {
                if (main_calls != (local ? 0u : 4u) || (manual && reviews != 2) ||
                    !state->control || state->control->phase != app::ControlPhase::released)
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
