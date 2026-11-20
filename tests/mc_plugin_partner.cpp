#include "application.hpp"
#include <dafeiyu/companion_abi_v2.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>

// 通过真实 DLL、命名管道与可控后端验证插件拥有一条完整的回复链路。
// 步骤：安装并启动原角色 → 等待在场和原话回合 → 核对真实技能回执 → 停止并卸载。
int wmain(int argc, wchar_t** argv) {
    using namespace dfy;
    using namespace model;
    try {
        const bool interrupt = argc == 4 && std::wstring_view(argv[3]) == L"partner-interrupt";
        const bool confirm = argc == 4 && std::wstring_view(argv[3]) == L"partner-confirm";
        const bool scene = argc == 4 && std::wstring_view(argv[3]) == L"partner-scene";
        const bool task = scene || (argc == 4 && std::wstring_view(argv[3]) == L"partner-task");
        if (argc != 3 && !interrupt && !confirm && !task) throw std::runtime_error("package and test directory required");
        const std::filesystem::path root = argv[2];
        std::atomic_bool actual_reply = false, assessed = false, interruption_seen = false;
        app::Application app({},root/"packages",root/"user",Backend{[&](const auto&,const Request& request) -> Response {
            const auto& last = request.messages.back();
            if (interrupt && request.tools.empty()) {
                const auto assessment = nlohmann::json::parse(last.content);
                assessed = assessment.at("newEvents").back().at("text") == "stop for now" &&
                    assessment.at("work").at("skill") == "move" && assessment.at("currentEvents").back().at("sequence") == 2;
                return {{Role::assistant,assessed ? "stop" : "continue"}};
            }
            if (last.role == Role::tool) {
                if (last.tool_call_id == "expand") {
                    const auto result = nlohmann::json::parse(last.content.substr(last.content.find('{')));
                    if (result.at("status") != "scheduled" || result.at("categories").size() != 12 || result.at("wake") != true)
                        throw std::runtime_error("discovery did not report the available categories");
                    return {{Role::assistant,"ready for expanded scope"}};
                }
                if (task && last.tool_call_id == "task") {
                    if (last.content.find("\"status\":\"updated\"") == std::string::npos)
                        throw std::runtime_error("shared task was not accepted");
                    if (scene) std::ofstream(root/"scene-changed").put('1');
                    return {{Role::assistant,"turn finished"}};
                }
                actual_reply = last.content.find(confirm ? "\"collected\":2" : "\"sent\":true") != std::string::npos;
                return {{Role::assistant,"internal settlement"}};
            }
            const auto input = nlohmann::json::parse(last.content);
            if (input.at("trigger") == "skills.expanded") {
                const auto wanted = confirm ? "_collect" : "_move";
                const auto chosen = std::ranges::find_if(request.tools,[&](const auto& tool) { return tool.name.ends_with(wanted); });
                if (chosen == request.tools.end() || request.tools.size() != 5 ||
                    !input.at("events").empty() || input.at("recentEvents").back().at("sequence") != 2)
                    throw std::runtime_error("expanded scope lost the original event or contains unintended skills");
                return {{Role::assistant,"",{{"chosen",chosen->name,confirm ? R"({"block":"dirt","count":2,"approval":"main"})" :
                    R"({"place":"player"})"}}},FinishReason::tool_calls};
            }
            if (task && input.at("trigger") == (scene ? "scene.changed" : "task.continue")) {
                if (!input.at("events").empty() || input.at("task").at("goal") != "resume the wall" ||
                    input.at("task").at("detail") != "check the corner" || input.at("task").at("revision") != 1 ||
                    (scene && input.at("sceneRevision") != 2))
                    throw std::runtime_error("autonomous turn lost the task or invented new chat");
                const auto reply_tool = std::ranges::find_if(request.tools,[](const auto& tool) { return tool.name.ends_with("_reply"); });
                return {{Role::assistant,"",{{"reply",reply_tool->name,R"({"text":"ready to play"})"}}},FinishReason::tool_calls};
            }
            if (input.at("events").back().at("type") == "player.entered") return {{Role::assistant,"silent baseline"}};
            if (interrupt && input.at("events").back().at("sequence") == 3) {
                interruption_seen = input.at("previousActivity").at("stopRequested") == true;
                return {{Role::assistant,"original work settled"}};
            }
            if (request.messages.size() != 4 || input.at("events").back().at("text") != "play with me")
                throw std::runtime_error("partner lost independent history or raw text");
            if (request.tools.size() != 4) throw std::runtime_error("initial scope leaked detailed actions");
            if (task) {
                const auto task_tool = std::ranges::find_if(request.tools,[](const auto& tool) { return tool.name.ends_with("_task_update"); });
                const auto args = nlohmann::json{{"revision",0},{"goal","resume the wall"},{"detail","check the corner"},
                    {"next",scene ? "scene" : "continue"}}.dump();
                return {{Role::assistant,"",{{"task",task_tool->name,args}}},FinishReason::tool_calls};
            }
            const auto tool = std::ranges::find_if(request.tools,[&](const auto& tool) {
                return tool.name.ends_with(confirm || interrupt ? "_skills" : "_reply");
            });
            if (tool == request.tools.end()) throw std::runtime_error("initial control skill missing");
            const auto args = confirm ? R"({"categories":["dig_break"]})" : interrupt ? R"({"categories":["movement"]})" :
                R"({"text":"ready to play"})";
            return {{Role::assistant,"",{{confirm || interrupt ? "expand" : "chosen",tool->name,args}}},FinishReason::tool_calls};
        }});
        app.plugins().install(argv[1]);
        if (app.plugins().load("mc.companion")) throw std::runtime_error("could not load MC package");
        const auto instance = app.plugins().instance("mc.companion");
        unsigned game_receipts = 0;
        unsigned decisions = 0;
        bool control_released = false, continuation_closed = false;
        nlohmann::json confirmation;
        const auto& mails = app.mailbox_host();
        // 回复原升级请求，投递编号由实际宿主信箱维护。
        const auto reply = [&](std::uint64_t number, bool approve, bool stale) {
            const auto payload = nlohmann::json{{"id",confirmation.at("id")},{"revision",1},{"player",confirmation.at("player")},
                {"generation",stale ? nlohmann::json("old-world") : confirmation.at("generation")},{"approve",approve}}.dump();
            mails.network->deliver(mails.identity,{instance->mailbox_addresses().inbound},number,
                events::mailbox_text("mc.confirmation.response",1,payload,1024));
        };
        // 记录原动作及业务裁定，确认接纳和动作执行来自不同消息。
        // 步骤：逐件读取原箱 → 处理升级请求及迟到答复 → 核对真实效果和释放。
        const auto drain = [&] {
            const auto address = events::MailboxId{instance->mailbox_addresses().outbound};
            for (;;) {
                auto mail = mails.network->read(mails.identity,address);
                if (!mail.event) break;
                const auto payload = mail.event.get().payload;
                const auto type = payload->descriptor()->name();
                const auto fact = nlohmann::json::parse(payload->find("text")->as_string());
                if (type == "mc.confirmation.request") {
                    confirmation = fact;
                    if (fact.at("skill") != "collect" || fact.at("arguments").at("count") != 2 ||
                        fact.at("arguments").contains("approval")) throw std::runtime_error("wrong original candidate");
                    reply(1,true,true);
                    reply(2,true,false);
                } else if (type == "mc.confirmation.decision") {
                    if (fact.at("status") != "approved" || fact.at("source") != "main" || fact.at("id") != confirmation.at("id"))
                        throw std::runtime_error("late answer changed original decision");
                    if (++decisions == 1) reply(3,false,false);
                } else if (type == "mc.partner.control") control_released = fact.at("phase") == "released";
                else if (task && type == "mc.partner.turn" && fact.at("order") == 2 && fact.at("request") == 3)
                    continuation_closed = fact.at("failed") == false;
                else if (type == "mc.partner.action") {
                    if (fact.at("order") != 2 || fact.at("invocation") == 0 || (confirm ? fact.at("result").at("collected") != 2 :
                        interrupt ? fact.at("result").at("status") != "interrupted" : fact.at("result").at("sent") != true))
                        throw std::runtime_error("game action mailbox lost the original exit receipt");
                    ++game_receipts;
                }
            }
        };
        {
            const auto api = instance->query<dfy_v2_companion_partner_api>(DFY_V2_COMPANION_PARTNER_INTERFACE,DFY_V2_COMPANION_PARTNER_VERSION);
            if (!api || instance->invoke(*api,[](const auto& fn, auto* native) { return fn.listen(native,nullptr); }))
                throw std::runtime_error("could not start plugin-owned partner");
            // 推进宿主直到原插件报告指定的状态边界。
            const auto wait = [&](bool stopped) {
                const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(8);
                while (std::chrono::steady_clock::now() < deadline) {
                    if (app.advance_plugins() == runtime::RunResult::failed) throw std::runtime_error("plugin loop failed");
                    drain();
                    dfy_v2_partner_status state{};
                    if (instance->invoke(*api,[&](const auto& fn, auto* native) { return fn.status(native,&state,nullptr); }))
                        throw std::runtime_error("partner state unavailable");
                    if (stopped ? state.phase == DFY_V2_PARTNER_CLOSED :
                        state.ready && state.session && state.settled == (interrupt ? 3u : 2u) && (!task || continuation_closed)) return;
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                throw std::runtime_error("plugin did not reach expected boundary");
            };
            wait(false);
            if ((interrupt ? !assessed || !interruption_seen : !actual_reply) || app.conversation().status())
                throw std::runtime_error("reply or interruption missing, or shared context");
            if (instance->invoke(*api,[](const auto& fn, auto* native) { return fn.stop(native,nullptr); }))
                throw std::runtime_error("could not stop plugin-owned partner");
            wait(true);
        }
        if (game_receipts != 1 || !control_released || (confirm && decisions != 2))
            throw std::runtime_error("expected original effect, confirmation or confirmed control release");
        if (app.plugins().unload("mc.companion") != plugins::ShutdownResult::closed) throw std::runtime_error("MC unload failed");
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
