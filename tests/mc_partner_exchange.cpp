#include "application.hpp"
#include <algorithm>
#include <atomic>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>

// 通过正式应用与原生插件验证本体委托、在途询问、真实动作及最后回报。
// 步骤：启动插件及固定模型 → 投递与复用原委托 → 在动作中询问 → 核对同任务的结算和退出。
int wmain(int argc, wchar_t** argv) {
    using namespace dfy;
    using namespace model;
    using namespace app;
    using Json = nlohmann::json;
    try {
        if (argc != 4) throw std::runtime_error("package, test directory and mode required");
        const std::filesystem::path root = argv[2];
        std::atomic_bool permit{false};
        std::atomic_uint delegated{0}, main_calls{0};
        Application host({},root/"packages",root/"user",Backend{[&](const auto&,const Request& input) -> Response {
            const auto& last = input.messages.back();
            if (std::ranges::any_of(input.tools,[](const auto& item) { return item.name == "read_game_receipts"; })) {
                ++main_calls;
                if (last.role == Role::tool) {
                    if (last.content.find("queued") == std::string::npos) throw std::runtime_error("mailbox tool omitted its receipt");
                    return {{Role::assistant,"request queued"}};
                }
                const bool start = last.content == "delegate from the main Agent";
                return {{Role::assistant,"",{{"mail",start ? "delegate_game_task" : "query_game_progress",
                    start ? R"({"text":"give a local greeting"})" : R"({"text":"where is the player now?"})"}}},FinishReason::tool_calls};
            }
            if (last.role == Role::tool) return {{Role::assistant,"report from partner"}};
            const auto body = Json::parse(last.content);
            if (!body.contains("delegation")) return {{Role::assistant,"game fact acknowledged"}};
            if (++delegated != 1 || body.at("delegation").at("text") != "give a local greeting" || !body.at("events").empty() ||
                input.tools.size() != 3 || std::ranges::any_of(input.tools,[](const auto& tool) {
                    return tool.name.ends_with("_move") || tool.name.ends_with("_collect");
                })) throw std::runtime_error("delegation lost source or widened authority");
            const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while (!permit && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!permit) throw std::runtime_error("progress query did not advance while child was running");
            const auto tool = std::ranges::find_if(input.tools,[](const auto& item) { return item.name.ends_with("_reply"); });
            if (tool == input.tools.end()) throw std::runtime_error("delegate reply tool unavailable");
            return {{Role::assistant,"",{{"delegate-reply",tool->name,R"({"text":"delegated hello"})"}}},FinishReason::tool_calls};
        }});
        host.plugins().install(argv[1]);
        if (host.plugins().load("mc.companion")) throw std::runtime_error("could not load MC package");
        const auto id = host.open_companion("mc.companion");
        host.listen_companion(id);
        std::uint64_t task = 0, inquiry = 0, cursor = 0;
        bool stopping = false, effect = false, saw_speech = false, asked_main = false, queried_main = false;
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(16);
        while (std::chrono::steady_clock::now() < deadline) {
            if (host.advance_plugins() == runtime::RunResult::failed) throw std::runtime_error("plugin failed during exchange");
            if (const auto main = host.conversation().advance(); main && main->error) std::rethrow_exception(main->error);
            const auto state = host.companion_status();
            const auto address = host.companion_address();
            const auto delta = host.companion_messages(id,cursor);
            saw_speech |= std::ranges::any_of(delta.messages,[](const auto& item) {
                    return item.kind == CompanionMessageKind::utterance;
                });
            if (!asked_main && address && state->settled_event == 2 && saw_speech) {
                host.submit_companion_exchange("delegate from the main Agent");
                asked_main = true;
            }
            if (task && !queried_main && delegated == 1 && address && !host.conversation().busy()) {
                host.submit_companion_exchange("request current status");
                queried_main = true;
            }
            for (const auto& item : delta.messages) {
                if (item.kind == CompanionMessageKind::delegation) {
                    task = item.revision;
                    if (host.send_companion(item.address,CompanionMessageKind::delegation,item.command,0,item.text) != task)
                        throw std::runtime_error("same request did not reuse its original identity");
                }
                if (item.kind == CompanionMessageKind::inquiry) inquiry = item.revision;
                if (item.kind == CompanionMessageKind::failed && (item.reply_to == task || item.reply_to == inquiry))
                    throw std::runtime_error("partner rejected exchange " + std::to_string(item.reply_to) + " " + item.text);
                if (item.kind == CompanionMessageKind::answer && item.reply_to == inquiry) {
                    const auto body = Json::parse(item.text);
                    if (body.at("task") != task || body.at("phase") != "running" || body.at("order") != 2 || body.at("observation").is_null())
                        throw std::runtime_error("current progress was not grounded in the original game feed");
                    permit = true;
                }
                if (item.kind == CompanionMessageKind::status && item.reply_to == task) {
                    const auto body = Json::parse(item.text);
                    effect |= body.at("skill") == "reply" && body.at("result").at("sent").get<bool>();
                }
                if (item.kind == CompanionMessageKind::report && item.reply_to == task) {
                    if (!effect || !permit || item.text != "report from partner") throw std::runtime_error("delegation hid its actual effect or result");
                    host.stop_companion(id);
                    stopping = true;
                }
            }
            cursor = delta.latest;
            if (stopping && state->closed) {
                if (main_calls != 4) throw std::runtime_error("main Agent did not complete both authorized rounds");
                host.conversation().close();
                if (host.close_plugins() != plugins::ShutdownResult::closed) throw std::runtime_error("game plugin failed to close");
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        throw std::runtime_error("original delegation did not settle, task="+std::to_string(task)+" inquiry="+std::to_string(inquiry)+
            " runs="+std::to_string(delegated.load())+" permit="+std::to_string(permit.load())+" cursor="+std::to_string(cursor));
    } catch (const std::exception& issue) { std::cerr << issue.what() << '\n'; return 1; }
}
