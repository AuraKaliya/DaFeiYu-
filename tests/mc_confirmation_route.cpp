#include "application.hpp"
#include <algorithm>
#include <atomic>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>

namespace {
// 核对原答复入队和插件收件是先后发生的两个阶段。
// 步骤：读取已落盘的路由记录 → 等待原收件回执 → 核对票据、顺序和原候选状态。
bool received(const std::filesystem::path& file, bool stale) {
    using Json = nlohmann::json;
    std::ifstream trace(file);
    Json delivery, acknowledgement, captured, decoded, dispatched;
    for (std::string line; std::getline(trace,line);) {
        const auto row = Json::parse(line);
        if (row.at("kind") == "interaction.delivered" && row.at("category") == "interaction.mail.reply") delivery = row;
        if (row.at("kind") == "interaction.acknowledged") acknowledgement = row;
        if (row.at("kind") == "event.captured" && row.at("type") == "mc.partner.help") captured = row;
        if (row.at("kind") == "event.decoded" && row.at("type") == "mc.partner.help") decoded = row;
        if (row.at("kind") == "event.dispatched" && !decoded.is_null() && row.at("eventDomain") == decoded.at("eventDomain") &&
            row.at("event") == decoded.at("event")) dispatched = row;
    }
    if (acknowledgement.is_null()) return false;
    if (delivery.is_null() || delivery.at("body").at("answer") != "approve" || delivery.at("ask") != acknowledgement.at("ask") ||
        delivery.at("seq") >= acknowledgement.at("seq") || acknowledgement.at("disposition") != (stale ? "stale" : "matched") ||
        delivery.at("message") != acknowledgement.at("message") || delivery.at("mailbox") != acknowledgement.at("mailbox") ||
        acknowledgement.at("legacyReceipt") != false)
        throw std::runtime_error("original answer delivery and plugin receipt were not independently verified");
    if (captured.is_null() || decoded.is_null() || dispatched.is_null() || captured.at("eventDomain") != decoded.at("eventDomain") ||
        captured.at("event") != decoded.at("event") || captured.at("seq") >= decoded.at("seq") ||
        decoded.at("seq") >= dispatched.at("seq") || dispatched.at("accepted") != true)
        throw std::runtime_error("capture, decode and dispatch were not independently verified");
    return true;
}
}

// 从正式应用验证主 Agent 选择渠道及游戏内本地确认，执行仍由原插件裁定。
// 步骤：注入固定角色策略 → 推进真实收发及主回合 → 从原渠道答复 → 核对真实候选结算并释放。
int wmain(int argc, wchar_t** argv) {
    using namespace dfy;
    using namespace model;
    using Json = nlohmann::json;
    try {
        if (argc != 4) throw std::runtime_error("package, test directory and route are required");
        const std::filesystem::path root = argv[2];
        const bool delayed_review = std::wstring_view(argv[3]) == L"partner-route-delayed";
        const bool manual = delayed_review || std::wstring_view(argv[3]) == L"partner-route-manual";
        const bool first = std::wstring_view(argv[3]) == L"partner-route-first";
        const bool panel = manual || std::wstring_view(argv[3]) == L"partner-route-panel";
        const bool local = std::wstring_view(argv[3]) == L"partner-route-local";
        const bool lose_terminal = std::wstring_view(argv[3]) == L"partner-route-lost-terminal";
        const bool retry_reply = std::wstring_view(argv[3]) == L"partner-route-retry-reply";
        std::atomic_uint main_calls = 0;
        std::atomic_uint reply_rounds = 0;
        std::atomic_bool terminal_disconnected = false;
        const auto host_start = std::chrono::steady_clock::now();
        std::atomic_int host_seconds = 0;
        app::ApplicationRunConfig run;
        run.tool_clock = [&] { return host_start+std::chrono::seconds(host_seconds.load()); };
        run.trace_directory = root;
        // 分别提供子 Agent 的原候选和主 Agent 的固定渠道选择。
        app::Application host({},root/"packages",root/"user",Backend{[&](const auto&,const Request& input) -> Response {
            const auto& last = input.messages.back();
            const auto routed = std::ranges::find_if(input.tools,[](const auto& tool) {
                return tool.name == "ask" || tool.name == "mail_reply";
            });
            if (routed != input.tools.end()) {
                ++main_calls;
                if (last.role == Role::tool) {
                    if (last.tool_call_id == "lost")
                        return {{Role::assistant,"",{{"present","ask",R"({"channel":"game"})"}}},FinishReason::tool_calls};
                    if (last.tool_call_id == "forged")
                        return {{Role::assistant,"",{{"reply","mail_reply","{}"},{"repeat","mail_reply","{}"}}},FinishReason::tool_calls};
                    return {{Role::assistant,"routing finished"}};
                }
                const auto envelope = Json::parse(last.content);
                const auto mail = Json::parse(envelope.at("content").get<std::string>());
                if (routed->name == "mail_reply") {
                    const auto round = ++reply_rounds;
                    const auto category = retry_reply && round > 1 ? "interaction.ask.completed.passive" : "interaction.ask.completed";
                    if (envelope.at("category") != category || mail.at("answer") != "approve")
                        throw std::runtime_error("main did not receive original answer");
                    if (retry_reply && round == 1) return {{Role::assistant,"I forwarded the answer."}};
                    if (retry_reply) return {{Role::assistant,"",{{"forged","mail_reply",R"({"answer":"deny"})"}}},FinishReason::tool_calls};
                    return {{Role::assistant,"",{{"reply","mail_reply","{}"}}},FinishReason::tool_calls};
                }
                const auto& context = mail.at("context");
                if (context.at("skill") != "collect" || context.at("arguments").at("count") != 2 ||
                    context.at("arguments").contains("approval") || envelope.at("category") != "mc.partner.question")
                    throw std::runtime_error("main did not receive original candidate");
                const auto expected = panel || lose_terminal ? Json::array({"game","terminal"}) : Json::array({"game"});
                if (Json::parse(routed->parameters).at("properties").at("channel").at("enum") != expected)
                    throw std::runtime_error("ASK exposed channels that did not match the entry point");
                if (lose_terminal) {
                    const auto limit = std::chrono::steady_clock::now()+std::chrono::seconds(2);
                    while (!terminal_disconnected && std::chrono::steady_clock::now() < limit) std::this_thread::yield();
                    if (!terminal_disconnected) throw std::runtime_error("test did not withdraw the terminal capability");
                    return {{Role::assistant,"",{{"lost","ask",R"({"channel":"terminal"})"}}},FinishReason::tool_calls};
                }
                return {{Role::assistant,"",{{"present","ask",panel ? R"({"channel":"terminal"})" : R"({"channel":"game"})"}}},
                    FinishReason::tool_calls};
            }
            if (input.tools.empty()) return {{Role::assistant,"continue"}};
            if (last.role == Role::tool) return {{Role::assistant,R"({"internal":"collection finished"})"}};
            const auto body = Json::parse(last.content);
            const auto expanded = body.at("trigger") == "skills.expanded";
            const auto& event = body.at(expanded ? "recentEvents" : "events").back();
            if (event.at("sequence") != 2) return {{Role::assistant,R"({"internal":"no game action"})"}};
            if (!expanded) {
                const auto directory = std::ranges::find_if(input.tools,[](const auto& tool) { return tool.name.ends_with("_skills"); });
                if (directory == input.tools.end()) throw std::runtime_error("skill directory unavailable");
                return {{Role::assistant,"",{{"expand",directory->name,R"({"categories":["dig_break"]})"}}},FinishReason::tool_calls};
            }
            const auto tool = std::ranges::find_if(input.tools,[](const auto& tool) { return tool.name.ends_with("_collect"); });
            if (tool == input.tools.end()) throw std::runtime_error("confirmed collection unavailable");
            const auto args = Json{{"block","dirt"},{"count",2},{"approval",local ? "game" : "main"}}.dump();
            return {{Role::assistant,"",{{"collect",tool->name,args}}},FinishReason::tool_calls};
        }},run);
        host.plugins().install(argv[1]);
        host.wake_for_plugin_mail(!manual);
        host.set_console_ask_available(panel || lose_terminal);
        if (host.plugins().load("mc.companion")) throw std::runtime_error("could not load MC package");
        const auto id = host.open_companion("mc.companion");
        host.listen_companion(id);
        bool requested_stop = false, delayed = false;
        unsigned manual_wait = 0, reviews = 0;
        const auto expected_calls = local ? 0u : retry_reply ? 6u : lose_terminal ? 5u : 4u;
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline) {
            if (lose_terminal && main_calls && !terminal_disconnected) {
                host.set_console_ask_available(false);
                terminal_disconnected = true;
            }
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
            if (manual && reviews == 1 && main_calls == 2 && !host.conversation().busy()) {
                // 跨过原答复截止及旧回收窗口，仍应取回同一份已接纳答复。
                if (delayed_review && !delayed) { host_seconds = 30; delayed = true; }
                else if (host.review_mail() == app::MailboxIntakeResult::submitted) ++reviews;
            }
            if (retry_reply && main_calls == 3 && !host.conversation().busy() && ++manual_wait == 30 &&
                host.review_mail() != app::MailboxIntakeResult::submitted)
                throw std::runtime_error("the original result was unavailable for explicit review");
            if (!requested_stop && state->settled_event == (panel ? 2u : first ? 4u : 3u) &&
                (local || (main_calls == expected_calls && !host.conversation().busy() && received(host.trace_file(),first || delayed_review)))) {
                host.stop_companion(id);
                requested_stop = true;
            }
            if (requested_stop && state->closed) {
                if (main_calls != expected_calls || (manual && reviews != 2) ||
                    !state->control || state->control->phase != app::ControlPhase::released)
                    throw std::runtime_error("channel, model routing or original game exit not verified");
                if (retry_reply) {
                    std::ifstream trace(host.trace_file());
                    unsigned intents = 0, deferred = 0;
                    for (std::string line; std::getline(trace,line);) {
                        const auto row = Json::parse(line);
                        if (row.at("kind") == "reply.intent" && row.at("callId") == "reply") ++intents;
                        if (row.at("kind") == "reply.review_required") ++deferred;
                    }
                    if (reply_rounds != 2 || intents != 1 || deferred != 1 || manual_wait != 30)
                        throw std::runtime_error("free text, invalid arguments or automatic retry changed the original intent");
                }
                host.conversation().close();
                if (host.close_plugins() != plugins::ShutdownResult::closed) throw std::runtime_error("game plugin failed to close");
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        throw std::runtime_error("confirmation did not reach its real exit");
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
