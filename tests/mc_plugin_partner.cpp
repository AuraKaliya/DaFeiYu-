#include "application.hpp"
#include "../plugins/mc/partner_effect.hpp"
#include "../plugins/mc/partner_knowledge.hpp"
#include "../plugins/mc/partner_task.hpp"
#include <dafeiyu/companion_abi_v2.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>

namespace {
// 核对草方块拆除、实际泥土入包以及部分成功的三个边界。
// 步骤：验证真实掉落物 → 验证仅破坏的独立结果 → 验证未完成不推进后续步骤。
void verify_collection_effect() {
    using dfy::mc::verified_action;
    auto result = nlohmann::json::parse(R"({"status":"done","broken":1,"changes":[{"before":"grass_block","after":"air"}],"inventoryDelta":{"dirt":1}})");
    if (!verified_action(result,"collect","") || !verified_action(result,"collect","dirt") ||
        verified_action(result,"collect","grass_block")) throw std::runtime_error("grass drop was not dirt");
    result["inventoryDelta"].clear();
    if (!verified_action(result,"collect","") || verified_action(result,"collect","dirt"))
        throw std::runtime_error("block removal and material pickup were conflated");
    result["status"] = "blocked";
    if (verified_action(result,"collect","")) throw std::runtime_error("partial collect may not advance a dependency");
}

// 保留知识的版本和条件，未知变种由执行器现场读取。
void verify_knowledge() {
    using dfy::mc::lookup_knowledge;
    const auto result = lookup_knowledge({{"query","Minecraft:GRASS_BLOCK 掉什么"}});
    const auto& facts = result.at("entries").at(0).at("facts");
    if (result.at("version") != "1.21.4" || facts.at("defaultDrop") != "dirt" || facts.at("silkTouchDrop") != "grass_block" ||
        lookup_knowledge({{"query","coarse_dirt"}}).at("status") != "unknown")
        throw std::runtime_error("knowledge lost its conditions or invented a rule");
}

// 核对已绑定目标和接近后绑定使用不同证据，拒绝用后来的玩家位置改写原决定。
// 步骤：锁定并尝试替换原格 → 登记接近请求 → 核对有效到达和实际空格。
void verify_target_evidence() {
    using Json = nlohmann::json;
    const Json position{{"x",1},{"y",-60},{"z",1}}, other{{"x",3},{"y",-60},{"z",1}};
    const Json anchor{{"dimension","overworld"},{"position",position}};
    const Json unknown{{"dimension","overworld"},{"position",nullptr}};
    const auto update = [&](dfy::mc::PartnerTask& task, const Json& source, Json target, std::uint64_t order = 0) {
        return task.update({{"goal","fill the original gap"},{"next","player"},{"target",std::move(target)}},anchor,0,&source,order)
            .at("status");
    };
    dfy::mc::PartnerTask fixed, approach;
    if (update(fixed,anchor,{{"message",2},{"cells",Json::array({position})}}) != "updated" ||
        update(fixed,anchor,{{"message",2},{"cells",Json::array({other})}}) != "invalid_target" ||
        update(fixed,anchor,{{"message",3},{"cells",Json::array({other})}},3) != "invalid_target")
        throw std::runtime_error("a later view overwrote the original target");
    if (update(fixed,anchor,{{"message",3},{"cells",Json::array({other})},{"replace",true}},3) != "updated")
        throw std::runtime_error("the current explicit replacement was not accepted");
    Json ground{{"source","ground"}};
    fixed.bind_collection(ground);
    if (ground.at("protectedCells") != Json::array({other})) throw std::runtime_error("gathering lost the bound target protection");
    const Json target{{"message",7},{"approach",true},{"observation","local"},{"cells",Json::array({position})}};
    if (update(approach,unknown,{{"message",7},{"approach",true}},7) != "updated" ||
        update(approach,unknown,target) != "invalid_target") throw std::runtime_error("unobserved approach was accepted");
    approach.record({{"skill","move"},{"order",7},{"result",{{"status","done"},{"target","player"},{"position",position}}}});
    const Json self{{"source","self"},{"status","sampled"},{"actor",{{"position",position}}}};
    auto readback = position;
    readback["block"] = "air";
    readback["atSelf"] = false;
    // 构造执行器按来源和坐标生成的已完成回执。
    const auto observe = [&] {
        const Json sample{{"verified",Json::array({readback})},{"views",Json::array({self})}};
        const Json observation{{"id","local"},{"dimension","overworld"},{"samples",Json::array({sample})}};
        approach.record({{"skill","observe"},{"result",{{"status","done"},{"observation",observation}}}});
    };
    observe();
    if (update(approach,unknown,target) != "invalid_target") throw std::runtime_error("live player was treated as self evidence");
    readback["atSelf"] = true;
    observe();
    auto args = position;
    args["block"] = "dirt";
    if (update(approach,unknown,target) != "updated" || !approach.bind_placement(args) || args.at("sourceMode") != "approach" ||
        args.at("sourceMessage") != 7) throw std::runtime_error("verified approach lost its original placement provenance");
}
}

// 通过真实 DLL、命名管道与可控后端验证插件拥有一条完整的回复链路。
// 步骤：安装并启动原角色 → 等待在场和原话回合 → 核对真实技能回执 → 停止并卸载。
int wmain(int argc, wchar_t** argv) {
    using namespace dfy;
    using namespace model;
    try {
        if (argc == 3) { verify_collection_effect(); verify_target_evidence(); verify_knowledge(); }
        const bool interrupt = argc == 4 && std::wstring_view(argv[3]) == L"partner-interrupt";
        const bool suspend = argc == 4 && std::wstring_view(argv[3]).starts_with(L"partner-follow-collect");
        const bool follow = suspend || (argc == 4 && std::wstring_view(argv[3]) == L"partner-follow");
        const bool motion = follow || (argc == 4 && std::wstring_view(argv[3]) == L"partner-motion");
        const bool confirm = argc == 4 && std::wstring_view(argv[3]) == L"partner-confirm";
        const bool departure = argc == 4 && std::wstring_view(argv[3]) == L"partner-departure";
        const bool scene = argc == 4 && std::wstring_view(argv[3]) == L"partner-scene";
        const bool task = scene || (argc == 4 && std::wstring_view(argv[3]) == L"partner-task");
        if (argc != 3 && !interrupt && !motion && !confirm && !task && !departure) throw std::runtime_error("package and test directory required");
        const std::filesystem::path root = argv[2];
        std::atomic_bool actual_reply = false, interruption_seen = false;
        std::atomic_bool left_observed = false, late_model = false;
        std::string follow_work, resumed_work;
        app::Application app({},root/"packages",root/"user",Backend{[&](const auto&,const Request& request) -> Response {
            if (request.messages.front().role != Role::system ||
                !request.messages.front().content.starts_with("你叫大肥鱼。") ||
                request.messages.front().content.find("\nMinecraft 同场规则\n") == std::string::npos)
                throw std::runtime_error("partner lost the host persona or its scene rules");
            const auto& last = request.messages.back();
            if (suspend && last.role == Role::tool && last.tool_call_id == "dig-scope") {
                const auto tool = std::ranges::find_if(request.tools,[](const auto& item) { return item.name.ends_with("_dig_block"); });
                if (tool == request.tools.end()) throw std::runtime_error("exact digging required discovery");
                return {{Role::assistant,"",{{"dig-one",tool->name,R"({"block":"grass_block","x":1,"y":-61,"z":2})"}}},
                    FinishReason::tool_calls};
            }
            if (last.role == Role::tool) {
                if (suspend) return {{Role::assistant,R"({"internal":"receipt stored"})"}};
                if (last.tool_call_id == "expand") {
                    const auto result = nlohmann::json::parse(last.content.substr(last.content.find('{')));
                    if (result.at("status") != "scheduled" || result.at("categories").size() != 12 || result.at("wake") != true)
                        throw std::runtime_error("discovery did not report the available categories");
                    return {{Role::assistant,R"({"internal":"ready for expanded scope"})"}};
                }
                if (task && last.tool_call_id == "task") {
                    if (last.content.find("\"status\":\"updated\"") == std::string::npos)
                        throw std::runtime_error("shared task was not accepted");
                    if (scene) std::ofstream(root/"scene-changed").put('1');
                    return {{Role::assistant,R"({"internal":"turn finished"})"}};
                }
                if (confirm && last.content.find("\"status\":\"waiting\"") == std::string::npos)
                    throw std::runtime_error("confirmation did not return a waiting item");
                if (!confirm && (!motion || last.tool_call_id == "reply"))
                    actual_reply = last.content.find("\"sent\":true") != std::string::npos;
                return {{Role::assistant,R"({"internal":"tool receipt read"})"}};
            }
            const auto input = nlohmann::json::parse(last.content);
            if (departure) {
                const auto& fact = input.at("events").back();
                if (fact.at("type") == "player.entered") return {{Role::assistant,""}};
                if (fact.at("type") == "player.left") { late_model = true; return {{Role::assistant,""}}; }
                std::ofstream(root/"turn-started").put('1');
                const auto limit = std::chrono::steady_clock::now()+std::chrono::seconds(3);
                while (!left_observed && std::chrono::steady_clock::now() < limit)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                return {{Role::assistant,"玩家离开后迟到的发言"}};
            }
            if (suspend) {
                using Json = nlohmann::json;
                // 为固定场景选择本轮确实开放的原生工具。
                const auto call = [&](std::string_view name, Json args) -> Response {
                    const auto tool = std::ranges::find_if(request.tools,[&](const auto& item) { return item.name.ends_with(name); });
                    if (tool == request.tools.end()) throw std::runtime_error("missing follow handoff tool");
                    return {{Role::assistant,"",{{std::string(name),tool->name,args.dump()}}},FinishReason::tool_calls};
                };
                const auto& events = input.at("events");
                if (!events.empty() && events.back().at("type") == "player.message") {
                    switch (events.back().at("sequence").get<int>()) {
                    case 2: return call("_follow",{{"distance",4}});
                    case 3: {
                        auto result = call("_task_update",{{"goal","Ground collection is the previous scope"},{"next","player"},
                            {"collection","ground"}});
                        result.message.tool_calls.front().id = "dig-scope";
                        return result;
                    }
                    case 4:
                        if (input.at("following").at("status") != "paused") throw std::runtime_error("follow is not paused");
                        return call("_reply",{{"text","I can hear you"}});
                    case 5:
                        if (input.at("following").at("segment") != 2) throw std::runtime_error("old segment was reused");
                        return call("_stop_motion",{{"work",input.at("following").at("work")}});
                    }
                }
                return {{Role::assistant,R"({"internal":"observed"})"}};
            }
            if (confirm && input.contains("finishedWork")) {
                if (input.at("finishedWork").at("result").at("collected") != 2)
                    throw std::runtime_error("confirmation lost the actual exit from the model context");
                actual_reply = true;
                return {{Role::assistant,R"({"internal":"game exit observed"})"}};
            }
            if (input.at("trigger") == "skills.expanded") {
                const auto wanted = confirm ? "_collect" : "_start_move";
                const auto chosen = std::ranges::find_if(request.tools,[&](const auto& tool) { return tool.name.ends_with(wanted); });
                if (chosen == request.tools.end() || request.tools.size() != 10 ||
                    !input.at("events").empty() || input.at("recentEvents").back().at("sequence") != 2)
                    throw std::runtime_error("expanded scope lost the original event or contains unintended skills");
                return {{Role::assistant,"",{{"chosen",chosen->name,confirm ? R"({"block":"dirt","count":2,"approval":"game"})" :
                    R"({"place":"player"})"}}},FinishReason::tool_calls};
            }
            if ((motion || interrupt) && input.at("trigger") == "motion.finished") {
                const auto& work = input.at("finishedWork");
                if (work.at("result").at("status") != "interrupted")
                    throw std::runtime_error("terminal motion receipt missing from subsequent context");
                if (interrupt) interruption_seen = work.at("order") == 2;
                return {{Role::assistant,"movement stopped"}};
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
            if (motion && input.at("events").back().at("sequence").get<unsigned>() >= 3) {
                const auto& active = input.at("activeMotion");
                if (active.at("order") != 2) throw std::runtime_error("active motion lost the original order");
                if (follow && (active.at("skill") != "follow" || active.at("progress").at("distance") != 4.5))
                    throw std::runtime_error("tracked progress missing from the active follow");
                const auto reply = input.at("events").back().at("sequence") == 3;
                if (reply && !follow) return {{Role::assistant,"still moving"}};
                const auto tool = std::ranges::find_if(request.tools,[&](const auto& item) {
                    return item.name.ends_with(reply ? "_reply" : "_stop_motion");
                });
                if (tool == request.tools.end()) throw std::runtime_error("motion presentation tool missing");
                const auto args = reply ? R"({"text":"still moving"})" : nlohmann::json{{"work",active.at("work")}}.dump();
                return {{Role::assistant,"",{{reply ? "reply" : "stop",tool->name,args}}},FinishReason::tool_calls};
            }
            if (input.at("events").back().at("sequence") == 2) {
                const auto& captured = input.at("events").back().at("sceneAtMessage");
                if (captured.at("sampledAtMs") != 42 || captured.at("dimension") != "overworld" ||
                    captured.at("position").at("x") != -0.5 || input.at("lastObservation").at("position").at("x") != -1.5)
                    throw std::runtime_error("original player scene was lost or replaced by the live actor position");
            }
            if (confirm && input.at("events").back().at("sequence") == 3) {
                if (input.at("pendingApproval").at("skill") != "collect" || input.contains("activeMotion"))
                    throw std::runtime_error("waiting approval was missing or already started");
                return {{Role::assistant,"I can hear you while waiting."}};
            }
            if (confirm && input.at("events").back().at("sequence") == 4)
                return {{Role::assistant,R"({"internal":"approval observed"})"}};
            if (interrupt && input.at("events").back().at("sequence") == 3) {
                const auto& active = input.at("activeMotion");
                const auto tool = std::ranges::find_if(request.tools,[](const auto& item) { return item.name.ends_with("_stop_motion"); });
                if (tool == request.tools.end() || active.at("order") != 2 || input.at("events").back().at("text") != "stop for now")
                    throw std::runtime_error("new message did not see the running move");
                return {{Role::assistant,"",{{"stop",tool->name,nlohmann::json{{"work",active.at("work")}}.dump()}}},FinishReason::tool_calls};
            }
            if (request.messages.size() != 2 || input.at("events").back().at("text") != "play with me")
                throw std::runtime_error("partner lost current raw text or carried previous model history");
            if (request.tools.size() != 9 || std::ranges::none_of(request.tools,[](const auto& tool) {
                return tool.name.ends_with("_queue_actions");
            })) throw std::runtime_error("initial scope lost queue control or leaked detailed actions");
            if (follow) {
                const auto tool = std::ranges::find_if(request.tools,[](const auto& item) { return item.name.ends_with("_follow"); });
                if (tool == request.tools.end()) throw std::runtime_error("follow missing from initial scope");
                return {{Role::assistant,"",{{"follow",tool->name,"{}"}}},FinishReason::tool_calls};
            }
            if (task) {
                const auto task_tool = std::ranges::find_if(request.tools,[](const auto& tool) { return tool.name.ends_with("_task_update"); });
                const auto args = nlohmann::json{{"goal","resume the wall"},{"detail","check the corner"},
                    {"next",scene ? "scene" : "continue"}}.dump();
                return {{Role::assistant,"",{{"task",task_tool->name,args}}},FinishReason::tool_calls};
            }
            const auto tool = std::ranges::find_if(request.tools,[&](const auto& tool) {
                return tool.name.ends_with(confirm || interrupt || motion ? "_skills" : "_reply");
            });
            if (tool == request.tools.end()) throw std::runtime_error("initial control skill missing");
            const auto args = confirm ? R"({"categories":["dig_break"]})" : interrupt || motion ? R"({"categories":["movement"]})" :
                R"({"text":"ready to play"})";
            return {{Role::assistant,"",{{confirm || interrupt || motion ? "expand" : "chosen",tool->name,args}}},FinishReason::tool_calls};
        }});
        app.plugins().install(argv[1]);
        if (app.plugins().load("mc.companion")) throw std::runtime_error("could not load MC package");
        const auto instance = app.plugins().instance("mc.companion");
        unsigned game_receipts = 0;
        unsigned decisions = 0;
        bool control_released = false, continuation_closed = false, left_recorded = false, turn_cancelled = false;
        std::string accepted_motion;
        const auto& mails = app.mailbox_host();
        // 记录原动作及业务裁定，确认接纳和动作执行来自不同消息。
        // 步骤：逐件读取原箱 → 关联本地游戏裁定 → 核对实际效果及释放。
        const auto drain = [&] {
            const auto address = events::MailboxId{instance->mailbox_addresses().outbound};
            for (;;) {
                auto mail = mails.network->read(mails.identity,address);
                if (!mail.event) break;
                const auto payload = mail.event.get().payload;
                const auto type = payload->descriptor()->name();
                const auto fact = nlohmann::json::parse(payload->find("text")->as_string());
                if (departure && type == "mc.partner.event" && fact.at("event").at("type") == "player.left") left_recorded = true;
                if (departure && type == "mc.partner.turn" && fact.at("order") == 2) turn_cancelled = fact.at("outcome") == "player_left";
                if (type == "mc.confirmation.decision") {
                    if (fact.at("status") != "approved" || fact.at("source") != "game" || fact.at("skill") != "collect")
                        throw std::runtime_error("original candidate did not receive the local approval");
                    ++decisions;
                } else if (type == "mc.partner.control") control_released = fact.at("phase") == "released";
                else if (suspend && type == "mc.partner.motion" && fact.at("status") == "accepted" &&
                    fact.at("action").at("skill") == "follow") {
                    const auto& action = fact.at("action");
                    if (follow_work.empty()) follow_work = action.at("work");
                    else if (action.at("work") != follow_work || action.at("bridgeWork") == follow_work ||
                        action.at("segment") != 2) throw std::runtime_error("follow identity changed after pause");
                    else resumed_work = action.at("bridgeWork");
                }
                else if (!suspend && motion && type == "mc.partner.motion") accepted_motion = fact.at("action").at("bridgeWork");
                else if (task && type == "mc.partner.turn" && fact.at("order") == 2 && fact.at("request") == 3)
                    continuation_closed = fact.at("failed") == false;
                else if (type == "mc.partner.action") {
                    if (suspend) {
                        const auto& result = fact.at("result");
                        if (fact.at("skill") == "follow") {
                            if (fact.at("work") != follow_work || result.at("status") != "interrupted" ||
                                fact.at("transition") != (resumed_work.empty() ? "paused" : "exited"))
                                throw std::runtime_error("follow did not exit its real execution segment");
                            ++game_receipts;
                        } else if (fact.at("skill") == "collect" && result.at("status") != "queued") ++game_receipts;
                        else if (fact.at("skill") == "reply" && result.at("sent") == true) ++game_receipts;
                        continue;
                    }
                    if (confirm && fact.at("skill") == "reply") {
                        if (fact.at("order") != 3 || fact.at("result").at("sent") != true || game_receipts)
                            throw std::runtime_error("chat did not exit before the original action");
                        continue;
                    }
                    if (motion) {
                        const bool reply = fact.at("skill") == "reply";
                        if (accepted_motion.empty() || fact.at("order") != (reply ? 3 : 2) ||
                            (reply ? fact.at("result").at("sent") != true || game_receipts != 0 ||
                                (fact.at("invocation") == 0) == follow :
                                fact.at("bridgeWork") != accepted_motion ||
                                fact.at("result").at("status") != "interrupted" || game_receipts != 1))
                            throw std::runtime_error("motion and reply receipts lost their original identities or order");
                        ++game_receipts;
                        continue;
                    }
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
                    if (departure && state.departed) left_observed = true;
                    if (stopped ? state.phase == DFY_V2_PARTNER_CLOSED : departure ? state.settled == 3 && left_recorded && turn_cancelled :
                        suspend ? state.settled == 5 && game_receipts == 4 :
                        state.ready && state.session && state.settled == (motion || confirm ? 4u : interrupt ? 3u : 2u) &&
                        (!task || continuation_closed) && (!motion || game_receipts == 2) && (!interrupt || interruption_seen) &&
                        (!confirm || (game_receipts == 1 && actual_reply))) return;
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                throw std::runtime_error("plugin did not reach expected boundary");
            };
            wait(false);
            if ((departure ? late_model.load() : suspend ? resumed_work.empty() : interrupt ? !interruption_seen :
                confirm ? game_receipts != 1 : motion ? accepted_motion.empty() : !actual_reply) ||
                app.conversation().status())
                throw std::runtime_error("reply or interruption missing, or shared context");
            if (instance->invoke(*api,[](const auto& fn, auto* native) { return fn.stop(native,nullptr); }))
                throw std::runtime_error("could not stop plugin-owned partner");
            wait(true);
        }
        if (game_receipts != (departure ? 0u : suspend ? 4u : motion ? 2u : 1u) || !control_released || (confirm && decisions != 1))
            throw std::runtime_error("expected original effect, confirmation or confirmed control release");
        if (app.plugins().unload("mc.companion") != plugins::ShutdownResult::closed) throw std::runtime_error("MC unload failed");
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
