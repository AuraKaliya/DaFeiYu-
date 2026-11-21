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
// 取出本次结算的实际主回执。
const nlohmann::json& primary_receipt(const nlohmann::json& context) {
    const auto& work = context.at("workSettlement");
    return work.at("receipts").at(work.at("primary").get<std::size_t>());
}

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
    if (fixed.revision() != 2 || update(fixed,anchor,{{"message",3},{"cells",Json::array({other})}}) != "updated" ||
        fixed.revision() != 2) throw std::runtime_error("binding the same target again changed the execution revision");
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
    if (update(approach,unknown,target) != "updated" || approach.revision() != 2 || !approach.bind_placement(args) || args.at("sourceMode") != "approach" ||
        args.at("sourceMessage") != 7) throw std::runtime_error("verified approach lost its original placement provenance");
}

// 重放实机的候选、水平误写和后来的简要观察，核对实际墙面方格。
// 步骤：登记第 8 条原话的竖墙 → 拒绝两种水平提交 → 用候选绑定竖墙。
void verify_opening_geometry() {
    using Json = nlohmann::json;
    using dfy::mc::PartnerTask;
    const Json anchor{{"dimension","overworld"},{"position",{{"x",3.67},{"y",-60},{"z",-6.97}}}};
    const Json gap{{"id","2,-60,-5:x"},{"x",2},{"y",-60},{"z",-5},{"axis","x"},{"width",2},{"height",2}};
    const Json floor = Json::array({{{"x",2},{"y",-60},{"z",-5}},{{"x",3},{"y",-60},{"z",-5}},
        {{"x",2},{"y",-60},{"z",-4}},{{"x",3},{"y",-60},{"z",-4}}});
    auto wall = floor;
    for (auto index : {2,3}) { wall[index]["z"] = -5; wall[index]["y"] = -59; }
    PartnerTask task;
    const auto observe = [&](Json focus) {
        task.record({{"skill","observe"},{"result",{{"status","done"},{"anchor",{{"message",8}}},
            {"sampledAtMs",20},{"playerFocus",std::move(focus)}}}});
    };
    observe({{"choice",gap.at("id")},{"candidates",Json::array({gap})}});
    observe({{"hit",nullptr}});
    Json update{{"goal","补墙"},{"next","continue"},{"target",{{"message",8},{"candidate",gap.at("id")},{"cells",floor}}}};
    if (task.update(update,anchor,0,&anchor).at("status") != "invalid_target")
        throw std::runtime_error("mixed candidate and handwritten coordinates were accepted");
    update["target"].erase("candidate");
    const auto conflict = task.update(update,anchor,0,&anchor);
    if (conflict.at("reason") != "cells_conflict_with_pointed_opening" || conflict.at("cells") != wall || task.revision())
        throw std::runtime_error("handwritten flat cells overrode the recorded wall");
    update["target"].erase("cells");
    update["target"]["candidate"] = gap.at("id");
    if (task.update(update,anchor,0,&anchor).at("status") != "updated" || task.target_cells() != wall)
        throw std::runtime_error("wall geometry was lost after hit observation");
}

// 验证任务修订与授权变化对同一在途计划的影响，保留可离线查看的宿主 Trace。
// 步骤：提交带依赖的计划 → 在首个动作接纳后更新任务或授权 → 核对原工作结果和后继状态。
int verify_plan_revision(const std::filesystem::path& package, const std::filesystem::path& root, std::wstring_view variant) {
    using namespace dfy;
    using namespace model;
    using Json = nlohmann::json;
    const bool notes = variant == L"partner-revision-notes";
    const bool approval = variant == L"partner-revision-approval";
    const bool goal = variant == L"partner-revision-goal";
    const bool correction = variant.starts_with(L"partner-revision-correction");
    const bool correction_lost = variant == L"partner-revision-correction-lost";
    const bool repair_observe = variant == L"partner-revision-repair-observe";
    const bool repair_plan = variant == L"partner-revision-repair-plan";
    const bool question_expiry = variant == L"partner-revision-question-expiry";
    const auto revision = notes || approval || correction ? 1 : 2;
    const Json wall = Json::array({{{"x",2},{"y",-60},{"z",-5}},{{"x",3},{"y",-60},{"z",-5}},
        {{"x",2},{"y",-59},{"z",-5}},{{"x",3},{"y",-59},{"z",-5}}});
    auto floor = wall;
    for (int i : {2,3}) { floor.at(i)["y"] = -60; floor.at(i)["z"] = -4; }
    const auto reason = approval ? "approval_changed" : "task_changed";
    std::atomic_bool checked = false;
    app::ApplicationRunConfig run;
    run.trace_directory = root;
    app::Application host({},root/"packages",root/"user",Backend{[&](const auto&, const Request& request) -> Response {
        // 固定响应仅使用本轮实际开放的工具名。
        const auto call = [&](std::string_view name, std::string id, const Json& args) -> Response {
            const auto tool = std::ranges::find_if(request.tools,[&](const auto& item) { return item.name.ends_with(name); });
            if (tool == request.tools.end()) throw std::runtime_error("revision probe missing required tool");
            return {{Role::assistant,"",{{std::move(id),tool->name,args.dump()}}},FinishReason::tool_calls};
        };
        const auto& last = request.messages.back();
        if (question_expiry) {
            if (last.role == Role::tool) {
                // 按固定调用编号读取同批返回的业务回执。
                const auto output = [&](std::string_view id) {
                    const auto tool = std::ranges::find(request.messages,id,&Message::tool_call_id);
                    if (tool == request.messages.end()) throw std::runtime_error("missing expiry tool result");
                    return Json::parse(tool->content.substr(tool->content.find('{')));
                };
                if (last.tool_call_id != "duplicate" || output("unanswered").at("reason") != "waiting_for_player" ||
                    output("actual-reply").at("sent") != true || output("duplicate").at("status") != "already_requested")
                    throw std::runtime_error("expiry authorized a change, woke the model early or repeated speech");
                checked = true;
                return {{Role::assistant,"这段内部结算不能发给玩家。"}};
            }
            const auto input = Json::parse(last.content);
            const auto& events = input.at("events");
            if (events.empty()) throw std::runtime_error("a silent player caused an autonomous model request");
            if (events.back().at("sequence") == 2)
                return call("_ask_player","local-question",{{"question","你指左边还是右边？"},{"options",{"左边","右边"}}});
            if (events.back().at("sequence") == 3) {
                if (input.at("pendingQuestion").at("status") != "expired" || input.contains("questionResult"))
                    throw std::runtime_error("expiry was consumed as an answer");
                auto batch = call("_dig_block","unanswered",{{"block","grass_block"},{"x",1},{"y",-61},{"z",2}});
                batch.message.tool_calls.push_back(call("_reply","actual-reply",{{"text","我在呢。"}}).message.tool_calls.front());
                batch.message.tool_calls.push_back(call("_reply","duplicate",{{"text","我还在这里等着呢。"}}).message.tool_calls.front());
                return batch;
            }
            return {{Role::assistant,R"({"internal":"ready"})"}};
        }
        if (repair_observe || repair_plan) {
            const auto skill = repair_observe ? "_observe" : "_queue_actions";
            const Json args = repair_observe ? Json{{"mode","scene"},{"block","dirt"}} :
                Json{{"operation","plan"},{"steps",Json::array({{{"id","bad"},{"skill","collect"},{"arguments",Json::object()}}})}};
            if (last.role == Role::tool) {
                const auto result = Json::parse(last.content.substr(last.content.find('{')));
                if (last.tool_call_id != "initial-invalid" || (repair_observe ?
                    result.at("reasonCode") != "observe_mixed_modes" :
                    result.at("reason") != "unsupported_plan_skill" || result.at("node") != "bad"))
                    throw std::runtime_error("failed parameter repair was retried or diagnostic was lost");
                checked = true;
                return call(skill,"repair",args);
            }
            if (Json::parse(last.content).at("events").back().at("sequence") == 2)
                return call(skill,"initial-invalid",args);
            return {{Role::assistant,R"({"internal":"ready"})"}};
        }
        if (last.role == Role::tool) {
            if (last.tool_call_id == "lost-correction") {
                const auto result = Json::parse(last.content.substr(last.content.find('{')));
                if (result.at("status") != "old_work_exit_unconfirmed" || !result.at("work").get_ref<const std::string&>().starts_with("plan."))
                    throw std::runtime_error("unconfirmed work did not hold the new target");
                checked = true;
                return {{Role::assistant,R"({"internal":"plan exit checked"})"}};
            }
            if (last.tool_call_id == "register-correction") {
                const auto result = Json::parse(last.content.substr(last.content.find('{')));
                if (result.at("status") != "correction_recorded" || result.at("correction").at("message") != 3)
                    throw std::runtime_error("the correction was not recorded from its original message");
            }
            if (last.tool_call_id == "observe-correction") {
                const auto result = Json::parse(last.content.substr(last.content.find('{')));
                return call("_task_update","commit-correction",{{"operation","commit_correction"},{"message",3},
                    {"goal","竖着补墙"},{"next","player"},{"target",{{"message",2},{"candidate","2,-60,-5:x"},
                        {"observation",result.at("observation").at("id")}}}});
            }
            if (last.tool_call_id == "commit-correction") {
                const auto result = Json::parse(last.content.substr(last.content.find('{')));
                const auto& task = result.at("task");
                if (task.at("revision") != 2 || task.contains("pendingCorrection") || task.at("target").at("message") != 2 ||
                    task.at("target").at("instruction") != 3 || task.at("target").at("cells") != wall ||
                    task.at("goal") != "竖着补墙") throw std::runtime_error("the corrected goal and cells did not switch together");
                return {{Role::assistant,R"({"internal":"wall rebound"})"}};
            }
            if (last.tool_call_id == "start-task") return call("_skills","allow-dig",{{"categories",{"dig_break"}}});
            if (last.tool_call_id == "allow-dig") {
                const Json first{{"id","first"},{"skill","dig_block"},
                    {"arguments",{{"block","grass_block"},{"x",1},{"y",-61},{"z",2}}}};
                auto second = first;
                second["id"] = "second";
                second["after"] = {"first"};
                second["arguments"]["x"] = 2;
                return call("_queue_actions","planned",{{"operation","plan"},{"steps",Json::array({first,second})}});
            }
            if (last.tool_call_id == "update-task") {
                const auto result = Json::parse(last.content.substr(last.content.find('{')));
                const auto& task = result.at("task");
                if (result.at("executionChanged") != !notes || task.at("revision") != revision ||
                    (notes && (task.at("detail") != "补记角落" || task.at("next") != "continue")))
                    throw std::runtime_error("task update did not classify the execution change");
                std::ofstream(root/"revision-updated").put('1');
            }
            return {{Role::assistant,R"({"internal":"receipt stored"})"}};
        }
        const auto input = Json::parse(last.content);
        if (input.at("trigger") == "plan.finished") {
            const auto& plan = input.at("taskPlan");
            const auto& layers = input.at("planSettlements");
            const auto& first = layers.at(0).at("nodes").at(0);
            const auto& second = layers.at(1).at("nodes").at(0).at("result");
            const auto& result = first.at("result");
            if (correction_lost) {
                if (plan.at("status") != "cancelled" || plan.at("revision") != 1 || second.at("status") != "skipped" ||
                    result.at("status") != "unconfirmed" || input.at("task").at("pendingCorrection").at("unconfirmedWork") != first.at("work"))
                    throw std::runtime_error("missing original unconfirmed work");
                return call("_task_update","lost-correction",{{"operation","commit_correction"},{"message",3},
                    {"goal","竖着补墙"},{"next","player"},{"target",{{"message",2},{"cells",wall}}}});
            }
            if (checked || plan.at("revision") != 1 || input.at("task").at("revision") != revision || layers.size() != 2 ||
                plan.at("status") != (notes ? "done" : "cancelled") ||
                first.at("work") != plan.at("nodes").at(0).at("work") || result.at("inventoryDelta").at("dirt") != 1 ||
                result.at("changes").at(0).at("x") != 1 || result.at("status") != (notes || goal ? "done" : "interrupted") ||
                second.at("status") != (notes ? "done" : "skipped") ||
                (!notes && (plan.at("cancelReason") != reason || second.at("reason") != reason)))
                throw std::runtime_error("plan did not retain its original executor exit and dependent outcome");
            if (correction) {
                const auto& task = input.at("task");
                if (!input.at("events").empty() || task.at("pendingCorrection").at("message") != 3 ||
                    task.at("target").at("cells") != floor) throw std::runtime_error("correction was lost at the old work boundary");
                return call("_observe","observe-correction",{{"sources",{"message","self"}},{"message",2},{"focus","openings"},{"verify",wall}});
            }
            checked = true;
            return {{Role::assistant,R"({"internal":"plan exit checked"})"}};
        } else if (correction && input.at("trigger") == "skills.expanded" && input.at("task").at("revision") == 2) {
            return call("_fill_target","fill-wall",{{"block","dirt"}});
        } else if (correction && input.at("trigger") == "motion.finished") {
            const auto& batch = input.at("workSettlement");
            const auto& result = batch.at("receipts").at(batch.at("primary").get<std::size_t>()).at("result");
            if (result.at("status") != "done" || result.at("cells").size() != 4 || result.at("changes").size() != 2 ||
                result.at("cells").at(0).at("status") != "present" || result.at("cells").at(2).at("status") != "placed")
                throw std::runtime_error("corrected wall did not settle cell by cell");
            checked = true;
            return {{Role::assistant,R"({"internal":"plan exit checked"})"}};
        } else if (!input.at("events").empty()) {
            const auto& event = input.at("events").back();
            if (event.at("sequence") == 2) {
                Json task{{"goal","清理门边"},{"next","player"}};
                if (correction) task["target"] = {{"message",2},{"cells",floor}};
                return call("_task_update","start-task",task);
            }
            if (event.at("sequence") == 3) {
                if (input.at("activeMotion").at("status") != "running") throw std::runtime_error("no accepted work at revision change");
                if (correction) return call("_task_update","register-correction",{{"operation","begin_correction"},{"message",3}});
                if (approval) return call("_dig_block","require-approval",{{"block","grass_block"},{"x",1},{"y",-61},{"z",2},{"approval","game"}});
                Json update{{"goal",goal ? "改清理另一处" : "清理门边"},{"next",notes ? "continue" : "player"}};
                if (notes) update["detail"] = "补记角落";
                if (!notes && !goal) update["collection"] = "ground";
                return call("_task_update","update-task",update);
            }
        }
        return {{Role::assistant,R"({"internal":"observed"})"}};
    }},std::move(run));
    host.plugins().install(package);
    if (host.plugins().load("mc.companion")) throw std::runtime_error("could not load plan package");
    const auto id = host.open_companion("mc.companion");
    host.listen_companion(id);
    // 等待验证回合真正退出，再关闭伴玩会话。
    const auto receipt_turn_finished = [&] {
        std::ifstream trace(host.trace_file());
        for (std::string line; std::getline(trace,line);)
            if (line.find("\"kind\":\"run.exited\"") != line.npos && line.find("\"result\":\"succeeded\"") != line.npos &&
                ((repair_observe || repair_plan) ? line.find("\"run\":2,") != line.npos :
                    line.find(question_expiry ? "这段内部结算" : "plan exit checked") != line.npos)) return true;
        return false;
    };
    bool stopping = false;
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(question_expiry ? 52 : 8);
    while (std::chrono::steady_clock::now() < deadline) {
        if (host.advance_plugins() == runtime::RunResult::failed) throw std::runtime_error("plan revision loop failed");
        if (stopping && host.companion_status()->closed) return 0;
        if (!stopping && checked && host.companion_status()->settled_event == (repair_observe || repair_plan ? 2u : 3u) && receipt_turn_finished()) {
            host.stop_companion(id);
            stopping = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error("plan revision did not reach the expected boundary");
}
}

// 通过真实 DLL、命名管道与可控后端验证插件拥有一条完整的回复链路。
// 步骤：安装并启动原角色 → 等待在场和原话回合 → 核对真实技能回执 → 停止并卸载。
int wmain(int argc, wchar_t** argv) {
    using namespace dfy;
    using namespace model;
    try {
        if (argc == 4 && std::wstring_view(argv[3]).starts_with(L"partner-revision-"))
            return verify_plan_revision(argv[1],argv[2],argv[3]);
        if (argc == 3) { verify_collection_effect(); verify_target_evidence(); verify_opening_geometry(); verify_knowledge(); }
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
            if (confirm && input.contains("workSettlement")) {
                if (primary_receipt(input).at("result").at("collected") != 2)
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
                const auto& work = primary_receipt(input);
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
