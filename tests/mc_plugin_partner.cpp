#include "application.hpp"
#include "../plugins/mc/follow_attention.hpp"
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

// 覆盖持续掉队、高差、丢失玩家与恢复的时间边界，以及相同异常的防重。
// 步骤：检查接近与重复采样 → 触发一次距离异常并恢复 → 检查高差、可见性及显式受阻。
void verify_follow_windows() {
    using dfy::mc::FollowAttention;
    using dfy::mc::FollowSignal;
    using Json = nlohmann::json;
    FollowAttention attention;
    const auto sample = [&](std::uint64_t at, double distance, double vertical, std::string_view phase, std::string_view path) {
        return attention.observe(Json{{"sampledAtMs",at},{"distance",distance},{"vertical",vertical},{"phase",phase},{"path",path}},3);
    };
    for (std::uint64_t at = 0; at <= 3500; at += 500)
        if (sample(at,14-static_cast<double>(at)/500,0,"following","reachable") != FollowSignal::none)
            throw std::runtime_error("normal closing movement raised attention");
    for (std::uint64_t at = 4000; at < 7000; at += 500)
        if (sample(at,at == 4000 ? 6 : 7,0,"following","reachable") != FollowSignal::none)
            throw std::runtime_error("brief distance fluctuation raised attention");
    if (sample(7000,7,0,"following","reachable") != FollowSignal::raised ||
        sample(7000,9,4,"blocked","unreachable") != FollowSignal::none ||
        attention.blocked(Json::object()) != FollowSignal::none || attention.reason() != "distance_stalled")
        throw std::runtime_error("distance episode did not deduplicate stronger or repeated samples");
    for (std::uint64_t at = 7500; at <= 8500; at += 500)
        if (sample(at,2.8,0,"in_range","reachable") != (at == 8500 ? FollowSignal::recovered : FollowSignal::none))
            throw std::runtime_error("follow recovery did not require stable near-range evidence");
    for (std::uint64_t at = 9000; at <= 11000; at += 500)
        if (sample(at,4,3,"following","unreachable") != (at == 11000 ? FollowSignal::raised : FollowSignal::none))
            throw std::runtime_error("vertical obstacle ignored the no-path evidence");
    if (attention.reason() != "unreachable_height" || attention.episode() != 2)
        throw std::runtime_error("separate vertical obstacle lost its own episode");

    attention = {};
    for (std::uint64_t at = 0; at <= 5000; at += 500)
        if (sample(at,0,0,"waiting_player","unknown") != (at == 5000 ? FollowSignal::raised : FollowSignal::none))
            throw std::runtime_error("missing entity was not verified over the expected interval");
    attention = {};
    if (sample(0,9,0,"following","unknown") != FollowSignal::none ||
        sample(3500,9,0,"following","unknown") != FollowSignal::none ||
        attention.blocked(Json::object()) != FollowSignal::raised)
        throw std::runtime_error("sample gap or explicit blocked exit used the wrong attention boundary");
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

// 检查原话的独立子句和导航修订边界。
// 步骤：区分取材和指向 → 接纳一次路线 → 核对新修订仍须沿用原墙。
void verify_route_and_dig_intent() {
    using Json = nlohmann::json;
    using dfy::mc::read_dig_intent;
    if (read_dig_intent("补上我面前这堵墙，从地上挖四块泥土").pointed ||
        read_dig_intent("这里有个原木，敲掉它，然后做成4个木板").material != "log" ||
        read_dig_intent("把这石块上的泥土敲掉").support != "stone")
        throw std::runtime_error("distinct clauses changed the original dig intent");

    dfy::mc::PartnerTask task;
    task.switch_task({{"operation","start_task"},{"kind","navigation"},{"goal","绕墙两圈"}},{{"sequence",2}});
    const auto survey = [&](std::string_view wall, int message) {
        task.record({{"skill","observe"},{"generation","world"},{"result",{{"route",{{"status","validated"},{"id","checked"},
            {"wall",wall},{"waypoints",Json::array()},{"observation","survey"},{"sourceMessage",message},{"generation","world"}}}}}});
    };
    survey("original",2);
    Json route{{"route","checked"},{"laps",2}};
    if (!task.bind_route(route)) throw std::runtime_error("original route was not bound");
    task.accept_route(route);
    task.finish_route(true);
    survey("original",2);
    if (task.bind_route(route)) throw std::runtime_error("a fresh survey restarted a settled revision");
    if (task.switch_task({{"operation","restart_task"},{"task",2},{"goal","另一面墙"}},{{"sequence",3}}).at("status") != "invalid_target" ||
        task.switch_task({{"operation","restart_task"},{"task",2}},{{"sequence",3}}).at("status") != "updated")
        throw std::runtime_error("restart did not distinguish the original goal");
    survey("other",3);
    if (task.bind_route(route)) throw std::runtime_error("restart inherited a different wall from the new view");
    survey("original",2);
    if (!task.bind_route(route) || route.at("taskRevision") != 2)
        throw std::runtime_error("restart did not retain the original wall under a new revision");
}

// 验证任务修订与授权变化对同一在途计划的影响，保留可离线查看的宿主 Trace。
// 步骤：提交带依赖的计划 → 在首个动作接纳后更新任务或授权 → 核对原工作结果和后继状态。
int verify_plan_revision(const std::filesystem::path& package, const std::filesystem::path& root, std::wstring_view variant) {
    using namespace dfy;
    using namespace model;
    using Json = nlohmann::json;
    const bool direct = variant.starts_with(L"partner-revision-direct-");
    const bool direct_place = variant.ends_with(L"-direct-place");
    const bool rejoin = variant.ends_with(L"-direct-rejoin");
    const bool notes = variant.ends_with(L"-notes");
    const bool approval = variant.ends_with(L"-approval");
    const bool goal = variant == L"partner-revision-goal" || direct_place;
    const bool correction = variant.starts_with(L"partner-revision-correction");
    const bool correction_lost = variant == L"partner-revision-correction-lost";
    const bool navigation = variant.starts_with(L"partner-revision-navigation");
    const bool restart_route = variant == L"partner-revision-navigation-restart";
    const bool mixed_route = variant == L"partner-revision-navigation-mixed";
    const bool repair_observe = variant == L"partner-revision-repair-observe";
    const bool repair_plan = variant == L"partner-revision-repair-plan";
    const bool repair_plan_observe = variant == L"partner-revision-repair-plan-observe";
    const bool layered = variant.starts_with(L"partner-revision-layered");
    const bool layered_blocked = variant == L"partner-revision-layered-blocked";
    const bool attention = variant == L"partner-revision-attention";
    const bool question_expiry = variant == L"partner-revision-question-expiry";
    const bool pointed_support = variant == L"partner-revision-pointed-support";
    const bool pointed_craft = variant.starts_with(L"partner-revision-pointed-craft");
    const bool no_pickup = variant == L"partner-revision-pointed-craft-no-pickup";
    const auto revision = notes || approval || correction ? 1 : 2;
    const Json wall = Json::array({{{"x",2},{"y",-60},{"z",-5}},{{"x",3},{"y",-60},{"z",-5}},
        {{"x",2},{"y",-59},{"z",-5}},{{"x",3},{"y",-59},{"z",-5}}});
    const Json outside{{"x",2},{"y",-58},{"z",-5},{"block","dirt"}};
    const Json outside_step{{"skill","place"},{"arguments",outside}};
    auto chosen = wall.at(0), unchosen = wall.at(1);
    chosen["block"] = unchosen["block"] = "dirt";
    const Json pads = Json::array({{{"x",3},{"y",-62},{"z",24}},{{"x",3},{"y",-61},{"z",25}}});
    auto floor = wall;
    for (int i : {2,3}) { floor.at(i)["y"] = -60; floor.at(i)["z"] = -4; }
    const auto reason = approval ? "approval_changed" : "task_changed";
    std::atomic_bool checked = false;
    unsigned attention_turns = 0;
    std::uint64_t placement_item = 0;
    std::string follow_id, old_work;
    app::ApplicationRunConfig run;
    run.trace_directory = root;
    app::Application host({},root/"packages",root/"user",Backend{[&](const auto&, const Request& request) -> Response {
        // 固定响应仅使用本轮实际开放的工具名。
        const auto call = [&](std::string_view name, std::string id, const Json& args) -> Response {
            const auto tool = std::ranges::find_if(request.tools,[&](const auto& item) { return item.name.ends_with(name); });
            if (tool == request.tools.end()) throw std::runtime_error("revision probe missing required tool");
            return {{Role::assistant,"",{{std::move(id),tool->name,args.dump()}}},FinishReason::tool_calls};
        };
        // 用同一个放置候选依次检查三个入口。
        // 步骤：直调使用原参数 → 队列和计划按各自契约提交一个相同步骤。
        const auto probe_placement = [&](std::string id, const Json& args) -> Response {
            if (id.ends_with("-direct")) return call("_place",std::move(id),args);
            const bool plan = id.ends_with("-plan");
            Json step{{"skill","place"},{"arguments",args}};
            if (plan) step["id"] = "placement";
            return call("_queue_actions",std::move(id),{{"operation",plan ? "plan" : "append"},{"steps",Json::array({step})}});
        };
        const auto& last = request.messages.back();
        if (attention) {
            if (last.role == Role::tool) return {{Role::assistant,R"({"internal":"ready"})"}};
            const auto input = Json::parse(last.content);
            const auto& trigger = input.at("trigger");
            if (trigger == "follow.attention") {
                const auto& alert = input.at("followAttention");
                follow_id = input.at("following").at("work").get<std::string>();
                if (++attention_turns != 1 || alert.at("work") != follow_id || alert.at("episode") != 1 ||
                    alert.at("reason") != "unreachable_height" || alert.at("progress").at("vertical") != 5)
                    throw std::runtime_error("follow samples caused duplicate wake or lost their game evidence");
                return {{Role::assistant,"我被高台挡住了，你等我找一下路。"}};
            }
            if (trigger == "motion.finished") {
                const auto& receipt = primary_receipt(input);
                if (follow_id.empty() || receipt.at("bridgeWork") != follow_id || receipt.at("work") != follow_id ||
                    receipt.at("result").at("status") != "interrupted")
                    throw std::runtime_error("stop did not read back the original follow work");
                checked = true;
                return {{Role::assistant,R"({"internal":"plan exit checked"})"}};
            }
            const auto& event = input.at("events").back();
            switch (event.at("sequence").get<int>()) {
            case 2: return call("_follow","start-follow",Json::object());
            case 3:
                if (input.at("following").at("work") != follow_id || input.at("followAttention").at("status") != "recovered" ||
                    input.at("activeMotion").at("progress").at("path") != "reachable")
                    throw std::runtime_error("new player chat could not see recovery of the same work");
                return {{Role::assistant,"听得见，我跟上了。"}};
            case 4: return call("_stop_motion","stop-follow",{{"work",follow_id}});
            }
            return {{Role::assistant,R"({"internal":"ready"})"}};
        }
        if (layered) {
            if (last.role == Role::tool) {
                const auto result = Json::parse(last.content.substr(last.content.find('{')));
                if (last.tool_call_id == "register") return call("_observe","survey",{{"sources",{"self"}},{"verify",pads}});
                if (last.tool_call_id == "survey")
                    return call("_task_update","bind",{{"goal","垫两级站位"},{"next","player"},{"target",{{"message",2},{"cells",pads},
                        {"observation",result.at("observation").at("id")}}}});
                if (last.tool_call_id != "bind" || result.at("status") != "updated")
                    throw std::runtime_error("layered plan made an intervening model request");

                Json nodes = Json::array();
                for (std::size_t i = 0; i < pads.size(); ++i) {
                    auto place = pads.at(i), stand = place;
                    place["block"] = "dirt";
                    stand["y"] = stand.at("y").get<int>()+1;
                    const auto previous = "stand-"+std::to_string(i);
                    const auto build = "place-"+std::to_string(i+1);
                    nodes.push_back({{"id",build},{"skill","place"},{"arguments",place}});
                    if (i) nodes.back()["after"] = {previous};
                    nodes.push_back({{"id","stand-"+std::to_string(i+1)},{"skill","move"},{"arguments",stand},{"after",{build}}});
                }
                return call("_queue_actions","layered-plan",{{"operation","plan"},{"steps",std::move(nodes)}});
            }
            const auto input = Json::parse(last.content);
            if (input.at("trigger") == "plan.finished") {
                const auto& plan = input.at("taskPlan");
                const auto& layers = input.at("planSettlements");
                if (checked || plan.at("status") != (layered_blocked ? "blocked" : "done") || layers.size() != 4)
                    throw std::runtime_error("layered plan did not settle all dependencies");
                for (std::size_t i = 0; i < layers.size(); ++i) {
                    const auto& node = layers.at(i).at("nodes").at(0);
                    const auto& result = node.at("result");
                    if (node.at("work") != plan.at("nodes").at(i).at("work") ||
                        result.at("status") != (!layered_blocked || i == 0 ? "done" : i == 1 ? "blocked" : "skipped") ||
                        ((!layered_blocked || i == 0) && (i%2 == 0 ? result.at("placed") != true : result.at("arrived") != true)))
                        throw std::runtime_error("layer lacked execution or skip proof under its original work ID");
                }
                checked = true;
                return {{Role::assistant,layered_blocked ? "第一块垫好了，但我站不上去。" : R"({"internal":"plan exit checked"})"}};
            }
            if (!input.at("events").empty() && input.at("events").back().at("sequence") == 2)
                return call("_task_update","register",{{"operation","start_task"},{"kind","work"},{"message",2},{"goal","垫两级站位"}});
            return {{Role::assistant,R"({"internal":"ready"})"}};
        }
        if (pointed_craft) {
            const Json continuation{{"item","planks"},{"count",4}};
            const std::string question = "你指哪块原木？";
            const std::string final = no_pickup ? "木头敲下来了，我还没捡到。" : "木头捡到了，我现在做不了木板。";
            if (last.role == Role::tool) {
                const auto result = Json::parse(last.content.substr(last.content.find('{')));
                if (last.tool_call_id == "first-focus") return call("_ask_player","which-log",{{"question",question},{"afterPickup",continuation}});
                if (last.tool_call_id == "answer-focus" || last.tool_call_id == "keep-successor") {
                    const auto original = std::ranges::find(request.messages,Role::user,&Message::role);
                    const auto item = Json::parse(original->content).at("pendingQuestion").at("item");
                    Json args{{"x",1},{"y",-59},{"z",2},{"block","oak_log"},{"message",4}};
                    if (last.tool_call_id == "answer-focus") {
                        args["resolves"] = item;
                        return call("_dig_block","keep-successor",args);
                    }
                    if (result.at("reasonCode") != "pointed_plan_required")
                        throw std::runtime_error("direct dig consumed a clarification with unfinished successors");
                    return call("_queue_actions","resume-original-plan",{{"operation","plan"},{"resolves",item},{"steps",Json::array({
                        {{"id","gather_log"},{"skill","dig_block"},{"arguments",std::move(args)}}})}});
                }
                if (last.tool_call_id == "final-line" || last.tool_call_id == "heard-player") {
                    if (result.at("sent") != true) throw std::runtime_error("player did not receive the one game reply");
                    checked = last.tool_call_id == "final-line";
                    return {{Role::assistant,checked ? R"({"internal":"plan exit checked"})" : R"({"internal":"heard"})"}};
                }
                throw std::runtime_error("unexpected model call while waiting for clarification or plan exit");
            }
            const auto input = Json::parse(last.content);
            if (input.at("trigger") == "plan.finished") {
                const auto& layers = input.at("planSettlements");
                const auto& nodes = input.at("taskPlan").at("nodes");
                const auto& first = layers.at(0).at("nodes").at(0);
                const auto& dig = first.at("result");
                const auto& craft = layers.at(1).at("nodes").at(0).at("result");
                if (checked || input.contains("pendingQuestion") || request.tools.size() != 1 ||
                    !request.tools.front().name.ends_with("_reply") || layers.size() != 2 ||
                    nodes.at(1).at("after") != Json::array({"gather_log"}) || nodes.at(0).at("requiresPickup") != "oak_log" ||
                    first.at("work") != nodes.at(0).at("work") || dig.at("changes").at(0).at("before") != "oak_log")
                    throw std::runtime_error("compound goal or original pickup work was lost at settlement");
                if (no_pickup ? dig.at("reasonCode") != "pickup_unverified" || craft.at("reason") != "prerequisite_failed" :
                    dig.at("inventoryDelta").at("oak_log") != 1 || craft.at("reasonCode") != "skill_unavailable" ||
                    craft.at("item") != "oak_planks" || craft.at("count") != 4)
                    throw std::runtime_error("crafting advanced without pickup or lost its requested product");
                return call("_reply","final-line",{{"text",final}});
            }
            const auto& event = input.at("events").back();
            const auto sequence = event.at("sequence").get<std::uint64_t>();
            if (sequence == 1) return {{Role::assistant,R"({"internal":"ready"})"}};
            if (sequence == 3 || sequence == 4) {
                const auto& pending = input.at("pendingQuestion");
                if (pending.at("question") != question || pending.at("afterPickup") != continuation ||
                    pending.at("originalText") != "这里有个原木，敲掉它，然后做成4个木板")
                    throw std::runtime_error("compound goal did not persist through ordinary chat");
                if (sequence == 3) return call("_reply","heard-player",{{"text","听得到。"}});
            }
            return call("_observe",sequence == 2 ? "first-focus" : "answer-focus",
                {{"mode","pointed"},{"message",sequence},{"focus","objects"}});
        }
        if (pointed_support) {
            const Json stone{{"x",1},{"y",-60},{"z",2},{"block","stone"}};
            const Json dirt{{"x",1},{"y",-59},{"z",2},{"block","dirt"}};
            if (last.role == Role::tool) {
                const auto result = Json::parse(last.content.substr(last.content.find('{')));
                if (last.tool_call_id == "bad-stone") {
                    if (result.at("reasonCode") != "pointed_block_unverified")
                        throw std::runtime_error("an unobserved direct request admitted the reference block");
                    return call("_observe","support-focus",{{"mode","pointed"},{"message",2},{"focus","objects"}});
                }
                if (last.tool_call_id == "support-focus")
                    return call("_queue_actions","bad-plan",{{"operation","plan"},{"steps",Json::array({
                        {{"id","wrong"},{"skill","dig_block"},{"arguments",stone}}})}});
                if (last.tool_call_id == "bad-plan") {
                    if (result.at("reason") != "pointed_block_unverified")
                        throw std::runtime_error("the ray hitting stone was mistaken for permission to dig it");
                    return call("_dig_block","remove-dirt",dirt);
                }
                throw std::runtime_error("the accepted pointed dig caused another model call before the exit receipt");
            }
            const auto input = Json::parse(last.content);
            if (input.at("trigger") == "motion.finished") {
                const auto& receipt = input.at("workSettlement").at("receipts").at(0).at("result");
                if (checked || receipt.at("changes").at(0).at("before") != "dirt" ||
                    receipt.at("inventoryDelta").at("dirt") != 1)
                    throw std::runtime_error("pointed dirt was not read back under its original work");
                checked = true;
                return {{Role::assistant,R"({"internal":"plan exit checked"})"}};
            }
            if (input.at("events").back().at("sequence") == 2) return call("_dig_block","bad-stone",stone);
            return {{Role::assistant,R"({"internal":"ready"})"}};
        }
        if (question_expiry) {
            if (last.role == Role::tool) {
                if (last.tool_call_id == "ack-before-question") {
                    if (last.content.find("\"sent\":true") == last.content.npos)
                        throw std::runtime_error("initial chat was not delivered before clarification");
                    return call("_ask_player","local-question",{{"question","你指左边还是右边？"},{"options",{"左边","右边"}}});
                }
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
                return call("_reply","ack-before-question",{{"text","我来看看。"}});
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
        if (repair_plan_observe) {
            const Response quiet{{Role::assistant,R"({"internal":"计划回执已核对"})"}};
            if (last.role == Role::tool) {
                const auto result = Json::parse(last.content.substr(last.content.find('{')));
                if (last.tool_call_id == "reject-observe-plan") {
                    if (result.at("reason") != "invalid_dependency" || result.at("retryAllowed") != true)
                        throw std::runtime_error("observation plan rejection was not recorded");
                    std::ofstream(root/"observe-plan-rejected").put('1');
                } else if (last.tool_call_id != "accept-observe-plan" || result.at("status") != "queued")
                    throw std::runtime_error("corrected observation plan was not admitted");
                return quiet;
            }

            const auto input = Json::parse(last.content);
            if (input.at("trigger") == "plan.finished") {
                const auto& plan = input.at("taskPlan");
                const auto& settled = input.at("planSettlements").at(0);
                const auto& node = settled.at("nodes").at(0);
                if (input.contains("planRejection") || plan.at("status") != "done" ||
                    settled.at("plan") != plan.at("id") || settled.at("status") != "settled" ||
                    node.at("work") != plan.at("nodes").at(0).at("work") || node.at("result").at("status") != "done")
                    throw std::runtime_error("observation plan did not settle on the original game work");
                std::ofstream(root/"observe-plan-settled").put('1');
                return quiet;
            }

            const auto& events = input.at("events");
            if (events.empty()) return quiet;
            const auto order = events.back().at("sequence").get<unsigned>();
            if (order == 4) {
                if (input.contains("planRejection") || !std::filesystem::exists(root/"observe-plan-settled"))
                    throw std::runtime_error("next message lost the settled observation plan");
                checked = true;
                return {{Role::assistant,"看过了，我还在。"}};
            }
            if (order != 2 && order != 3) return quiet;
            Json node{{"id","nearby"},{"skill","observe"},{"arguments",Json::object()}};
            if (order == 2) node["after"] = {"missing"};
            else if (input.at("planRejection").at("result").at("reason") != "invalid_dependency")
                throw std::runtime_error("repair turn did not see the original rejection");
            return call("_queue_actions",order == 2 ? "reject-observe-plan" : "accept-observe-plan",
                {{"operation","plan"},{"steps",Json::array({node})}});
        }
        if (repair_observe || repair_plan) {
            const auto skill = repair_observe ? "_observe" : "_queue_actions";
            const Json args = repair_observe ? Json{{"mode","scene"},{"block","dirt"}} :
                Json{{"operation","plan"},{"steps",Json::array({{{"id","bad"},{"skill","collect"},{"arguments",Json::object()}}})}};
            if (last.role == Role::tool) {
                const auto result = Json::parse(last.content.substr(last.content.find('{')));
                if (repair_plan && last.tool_call_id == "repair") {
                    if (result.at("retryAllowed") != false || result.at("reason") != "unsupported_plan_skill" ||
                        !result.at("attempt").is_number_unsigned() || !result.at("planNodes").is_array())
                        throw std::runtime_error("plan rejection did not settle a bounded attempt");
                    checked = true;
                    return {{Role::assistant,"这次没动起来，我得换个办法。"}};
                }
                if (last.tool_call_id == "bad-category") {
                    if (result.at("status") != "invalid_category" || result.at("category") != "navigation" ||
                        std::ranges::find(result.at("validCategories"),"movement") == result.at("validCategories").end() ||
                        std::ranges::any_of(result.at("planNodes"),[](const auto& node) { return node.at("skill") == "collect"; }))
                        throw std::runtime_error("category error did not expose the real action directory");
                    return call(skill,"initial-invalid",args);
                }
                if (last.tool_call_id != "initial-invalid" || (repair_observe ?
                    result.at("reasonCode") != "observe_mixed_modes" :
                    result.at("reason") != "unsupported_plan_skill" || result.at("node") != "bad"))
                    throw std::runtime_error("failed parameter repair was retried or diagnostic was lost");
                if (repair_plan && result.at("retryAllowed") != true)
                    throw std::runtime_error("first invalid plan was not eligible for repair");
                checked = repair_observe;
                return call(skill,"repair",args);
            }
            if (Json::parse(last.content).at("events").back().at("sequence") == 2) {
                if (repair_plan) return call("_skills","bad-category",{{"categories",{"navigation"}}});
                return call(skill,"initial-invalid",args);
            }
            return {{Role::assistant,R"({"internal":"ready"})"}};
        }
        if (last.role == Role::tool) {
            if (navigation && last.tool_call_id == "wall-survey") {
                const auto result = Json::parse(last.content.substr(last.content.find('{')));
                return call("_observe","read-route",{{"mode","route"},{"observation",result.at("wallSurvey").at("id")}});
            }
            if (navigation && last.tool_call_id == "read-route") {
                const auto result = Json::parse(last.content.substr(last.content.find('{')));
                const auto& route = result.at("route");
                Json steps = Json::array();
                if (mixed_route) steps.push_back({{"id","approach"},{"skill","move"},{"arguments",route.at("waypoints").at(0)}});
                steps.push_back({{"id","circle"},{"skill","route"},{"arguments",{{"route",route.at("id")},{"laps",2}}}});
                if (mixed_route) steps.back()["after"] = {"approach"};
                return call("_queue_actions","route-plan",{{"operation","plan"},{"steps",std::move(steps)}});
            }
            if (last.tool_call_id == "switch-to-route") {
                const auto result = Json::parse(last.content.substr(last.content.find('{')));
                const auto& task = result.at("task");
                if (task.at("id") != 3 || task.at("revision") != (restart_route ? 1 : 2) || task.at("kind") != "navigation" ||
                    task.contains("target") || task.contains("lastAction") ||
                    (!restart_route && task.at("previousTarget").at("cells") != floor))
                    throw std::runtime_error("new task inherited the old construction goal");
                if (restart_route) return call("_observe","wall-survey",{{"mode","wall"},{"message",3}});
                return call("_dig_block","stale-execution",{{"block","dirt"},{"x",1},{"y",-61},{"z",2}});
            }
            if (restart_route && last.tool_call_id == "restart-route") {
                const auto result = Json::parse(last.content.substr(last.content.find('{')));
                const auto& task = result.at("task");
                if (task.at("id") != 3 || task.at("revision") != 2 || task.at("instruction") != 5 ||
                    task.at("navigationWall").at("message") != 3 || task.at("navigationWall").at("laps") != 2 ||
                    task.contains("lastAction")) throw std::runtime_error("restart lost the original wall or included its old action");
            }
            if (last.tool_call_id == "stale-execution") {
                const auto result = Json::parse(last.content.substr(last.content.find('{')));
                const auto barrier = result.at("reason");
                if (barrier != "waiting_for_old_work")
                    throw std::runtime_error("old front action bypassed the task handoff");
            }
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
            if (last.tool_call_id == "start-task" && !direct_place) return call("_skills","allow-dig",{{"categories",{"dig_break"}}});
            if (last.tool_call_id == "allow-dig" && !direct) {
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
                if (direct && !notes)
                    return call("_gather_ground","early-work",{{"item","dirt"},{"requiredStock",2}});
            }
            if (last.tool_call_id == "early-work") {
                const auto result = Json::parse(last.content.substr(last.content.find('{')));
                if (result.value("reason","") != "waiting_for_old_work")
                    throw std::runtime_error("new foreground work entered before the old exit reached the turn");
            }
            const auto& id = last.tool_call_id;
            if (direct_place && (id.starts_with("outside-") || id.starts_with("waiting-") || id.starts_with("unchosen-"))) {
                const auto result = Json::parse(last.content.substr(last.content.find('{')));
                const bool out = id.starts_with("outside-"), waiting = id.starts_with("waiting-");
                const auto prefix = id.substr(0,id.find('-'));
                const auto expected_reason = out ? "placement_outside_target" : waiting ? "waiting_for_player" : "answer_target_mismatch";
                if (result.value("status","") != (id == "outside-plan" ? "invalid_plan" : "blocked") ||
                    result.value("reason","") != expected_reason || (!out && result.at("item") != placement_item))
                    throw std::runtime_error(id+" returned an inconsistent placement decision: "+result.dump());

                const auto& candidate = out ? outside : waiting ? chosen : unchosen;
                if (id.ends_with("-append")) return probe_placement(prefix+"-plan",candidate);
                if (id.ends_with("-plan")) return probe_placement(prefix+"-direct",candidate);
                if (id == "outside-direct") return call("_ask_player","choose-cell",{{"question","补左边还是右边？"},
                    {"placementOptions",Json::array({{{"answer","左边"},{"cells",Json::array({wall.at(0)})}},
                        {{"answer","右边"},{"cells",Json::array({wall.at(1)})}}})}});
                if (id == "unchosen-direct") return call("_place","direct-work",chosen);
                if (id == "outside-replace")
                    return call("_task_update","update-task",{{"goal","改清理另一处"},{"next","player"},{"target",Json::object()}});
                std::ofstream(root/"placement-waiting-checked").put('1');
            }
            return {{Role::assistant,R"({"internal":"receipt stored"})"}};
        }
        const auto input = Json::parse(last.content);
        if (direct && input.at("trigger") == "skills.expanded") {
            if (direct_place)
                return probe_placement("outside-append",outside);
            return call("_collect","direct-work",{{"block","grass_block"},{"count",2}});
        }
        if (direct && (input.at("trigger") == "task.ready" || input.at("trigger") == "motion.finished")) {
            const auto& task = input.at("task");
            const auto& action = notes ? input.at("workSettlement").at("receipts").front() : input.at("taskHandoff").at("action");
            if (checked || old_work.empty() || action.at("bridgeWork") != old_work || action.at("taskRevision") != 1 ||
                task.at("revision") != revision ||
                (task.contains("lastAction") && task.at("lastAction").at("bridgeWork") == old_work) ||
                action.at("result").at("status") != (notes ? "done" : "interrupted") ||
                action.at("result").at("changes").size() != (notes ? 2u : 1u) ||
                (!notes && input.at("taskHandoff").at("status") != "exited"))
                throw std::runtime_error("direct work did not retain its original actual exit");
            checked = true;
            return {{Role::assistant,R"({"internal":"plan exit checked"})"}};
        }
        if (navigation && input.at("trigger") == "task.ready") {
            const auto& exit = input.at("taskHandoff");
            const auto& action = exit.at("action");
            if (checked || input.contains("taskPlan") || input.contains("workSettlement") || input.contains("planSettlements") ||
                exit.at("work") != action.at("bridgeWork") || action.at("result").at("status") != "interrupted" ||
                action.at("taskRevision") != 1)
                throw std::runtime_error("old work did not settle separately by original ID");
            if (restart_route) {
                const auto& target = input.at("task").at("navigationWall");
                if (action.at("skill") != "route" || action.at("result").at("route").at("laps") != 1 ||
                    input.at("following").at("status") != "paused" || input.contains("pendingQuestion") ||
                    input.at("questionResult").at("status") != "superseded")
                    throw std::runtime_error("old route or local question did not exit while follow remained paused");
                return call("_observe","read-route",{{"mode","route"},{"observation",target.at("observation")},{"wall",target.at("id")}});
            }
            if (action.at("result").at("inventoryDelta").at("dirt") != 1)
                throw std::runtime_error("old collect lost its pickup readback");
            return call("_observe","wall-survey",{{"mode","wall"},{"message",3}});
        }
        if (input.at("trigger") == "plan.finished") {
            const auto& plan = input.at("taskPlan");
            const auto& layers = input.at("planSettlements");
            const auto& first = layers.at(0).at("nodes").at(0);
            if (navigation) {
                const auto route_index = mixed_route ? 1u : 0u;
                const auto& node = layers.at(route_index).at("nodes").at(0);
                const auto& route = node.at("result").at("route");
                if (checked || plan.at("status") != "done" || layers.size() != route_index+1 || plan.at("nodes").size() != route_index+1 ||
                    node.at("work") != plan.at("nodes").at(route_index).at("work") || route.at("laps") != 2 ||
                    route.at("checkpoints").size() != 9 || input.at("task").at("next") != "done" ||
                    (mixed_route ? first.at("result").at("arrived") != true :
                        request.tools.size() != 1 || !request.tools.front().name.ends_with("_reply")))
                    throw std::runtime_error("route checkpoints woke the model separately or did not settle by plan ID");
                checked = true;
                return {{Role::assistant,R"({"internal":"plan exit checked"})"}};
            }
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
                if (restart_route) return call("_follow","follow",{{"distance",4}});
                Json task{{"goal","清理门边"},{"next","player"}};
                if (correction || navigation || direct_place) task["target"] = {{"message",2},{"cells",floor}};
                return call("_task_update","start-task",task);
            }
            if (rejoin && event.at("sequence") == 5) {
                if (request.messages.size() != 2 || input.at("events").size() != 2 ||
                    input.at("events").front().at("type") != "player.entered" || input.at("task").at("revision") != 0 ||
                    event.at("text") != "我回来了，先聊一会" || input.contains("recentDialogue") ||
                    input.contains("activeMotion") || input.contains("pendingQuestion"))
                    throw std::runtime_error("new presence reused the retired task or child context");
                checked = true;
                return {{Role::assistant,R"({"internal":"plan exit checked"})"}};
            }
            if (direct_place && event.at("sequence") == 3) {
                const auto& question = input.at("pendingQuestion");
                if (question.at("status") != "waiting") throw std::runtime_error("placement question did not stay open");
                placement_item = question.at("item").get<std::uint64_t>();
                return probe_placement("waiting-append",chosen);
            }
            if (direct_place && event.at("sequence") == 4) {
                const auto& answer = input.at("questionResult");
                if (answer.at("item") != placement_item || answer.at("placementCells") != Json::array({wall.at(0)}))
                    throw std::runtime_error("placement answer did not identify the chosen cell");
                return probe_placement("unchosen-append",unchosen);
            }
            if (event.at("sequence") == (direct_place ? 5 : 3)) {
                if (input.at("activeMotion").at("status") != "running") throw std::runtime_error("no accepted work at revision change");
                if (direct) old_work = input.at("activeMotion").at("work");
                if (direct_place) return call("_queue_actions","outside-replace",{{"operation","replace"},{"work",old_work},
                    {"steps",Json::array({{{"skill","move"},{"arguments",wall.front()}},outside_step})}});
                if (navigation) return call("_task_update","switch-to-route",{{"operation","start_task"},{"message",3},
                    {"kind","navigation"},{"goal","绕着这面墙跑两圈"}});
                if (correction) return call("_task_update","register-correction",{{"operation","begin_correction"},{"message",3}});
                if (approval) return call("_dig_block","require-approval",{{"block","grass_block"},{"x",1},{"y",-61},{"z",2},{"approval","game"}});
                Json update{{"goal",goal ? "改清理另一处" : "清理门边"},{"next",notes ? "continue" : "player"}};
                if (notes) update["detail"] = "补记角落";
                if (!notes && !goal) update["collection"] = "ground";
                return call("_task_update","update-task",update);
            }
            if (restart_route && event.at("sequence") == 4) {
                if (input.at("activeMotion").at("progress").at("laps") != 0)
                    throw std::runtime_error("first route did not expose background progress to player chat");
                return call("_ask_player","timing",{{"question","要从这圈开始计时吗？"},{"options",{"计时","不用"}}});
            }
            if (restart_route && event.at("sequence") == 5) {
                if (input.at("activeMotion").at("progress").at("laps") != 1 || !input.contains("pendingQuestion"))
                    throw std::runtime_error("restart was not issued mid-route");
                return call("_task_update","restart-route",{{"operation","restart_task"},{"message",5},{"task",3}});
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
                    line.find(repair_plan_observe ? "看过了，我还在。" : question_expiry ? "这段内部结算" :
                        layered_blocked ? "第一块垫好了" : "plan exit checked") != line.npos)) return true;
        return false;
    };
    bool stopping = false;
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(question_expiry ? 52 : 8);
    while (std::chrono::steady_clock::now() < deadline) {
        if (host.advance_plugins() == runtime::RunResult::failed) throw std::runtime_error("plan revision loop failed");
        if (stopping && host.companion_status()->closed) return 0;
        if (!stopping && checked && host.companion_status()->settled_event ==
            (repair_observe || repair_plan || pointed_support || layered ? 2u : restart_route || rejoin || direct_place ? 5u :
                pointed_craft || attention || repair_plan_observe ? 4u : 3u) &&
            receipt_turn_finished() &&
            (!restart_route || std::filesystem::exists(root/"original-follow-resumed"))) {
            host.stop_companion(id);
            stopping = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error("plan revision did not reach the expected boundary");
}

// 验证不可执行时仍可推进私有读口，且不唤醒模型或接管角色。
// 步骤：装入伙伴入口 → 等待原话水位 → 核对实际状态 → 关闭本地会话。
int verify_unavailable_partner(const std::filesystem::path& package, const std::filesystem::path& root, bool scenario) {
    using namespace dfy;
    std::atomic_uint requests = 0;
    app::Application host({},root/"packages",root/"user",model::Backend{[&](const auto&,const model::Request&) -> model::Response {
        ++requests;
        return {{model::Role::assistant,""}};
    }});
    host.plugins().install(package);
    if (host.plugins().load("mc.companion")) throw std::runtime_error("could not load partner package");
    const auto id = host.open_companion("mc.companion");
    host.listen_companion(id);
    bool stopping = false;
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (host.advance_plugins() == runtime::RunResult::failed) throw std::runtime_error("unavailable partner loop failed");
        if (stopping && host.companion_status()->closed) {
            if (requests || host.companion_status()->control) throw std::runtime_error("read-only partner attempted execution");
            return 0;
        }
        if (!stopping) {
            const auto input = host.companion_inbox(id);
            if (input->ready || host.companion_status()->closed) throw std::runtime_error("readable bridge was treated as ready or stopped");
            if (input->cursor == 2) {
                if (!input->readable || input->execution_mode != (scenario ? "scenario" : "partner") ||
                    input->execution_state != (scenario ? "ready" : "released"))
                    throw std::runtime_error("execution status lost its mode or reason");
                host.stop_companion(id);
                stopping = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error("readable bridge status did not advance");
}

// 用固定响应核对显式发言与最终文本在不同投递终态下的结算。
// 步骤：只交付一条玩家原话 → 返回显式发言及最终文本 → 等原回复结算并关闭。
int verify_reply_delivery(const std::filesystem::path& package, const std::filesystem::path& root, std::wstring_view variant) {
    using namespace dfy;
    using namespace model;
    using Json = nlohmann::json;
    const auto outcome = variant.ends_with(L"-sent") ? "sent" : variant.ends_with(L"-unknown") ? "unknown" : "not_sent";
    const bool internal = variant.ends_with(L"-internal");
    const std::string reply = "我在这儿。";
    std::atomic_bool received = false;
    app::ApplicationRunConfig run;
    run.trace_directory = root;
    app::Application host({},root/"packages",root/"user",Backend{[&](const auto&, const Request& request) -> Response {
        const auto& message = request.messages.back();
        if (message.role == Role::tool) {
            const auto result = Json::parse(message.content.substr(message.content.find('{')));
            if (result.at("delivery") != outcome || received.exchange(true)) throw std::runtime_error("lost actual reply delivery state");
            return {{Role::assistant,internal ? R"({"internal":"keep quiet"})" : reply}};
        }
        const auto input = Json::parse(message.content);
        if (input.at("events").back().at("sequence") == 1) return {{Role::assistant,""}};
        const auto tool = std::ranges::find_if(request.tools,[](const auto& entry) { return entry.name.ends_with("_reply"); });
        if (received || tool == request.tools.end()) throw std::runtime_error("reply was unavailable or the turn repeated");
        const Json args{{"text",std::string_view(outcome) == "not_sent" ? "第一行。\n第二行。" : reply}};
        return {{Role::assistant,"",{{"explicit-reply",tool->name,args.dump()}}},FinishReason::tool_calls};
    }},std::move(run));
    host.plugins().install(package);
    if (host.plugins().load("mc.companion")) throw std::runtime_error("could not load reply package");
    const auto id = host.open_companion("mc.companion");
    host.listen_companion(id);
    bool stopping = false;
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < deadline) {
        if (host.advance_plugins() == runtime::RunResult::failed) throw std::runtime_error("reply loop failed");
        if (stopping && host.companion_status()->closed) return 0;
        if (!stopping && received && host.companion_status()->settled_event == 2) {
            host.stop_companion(id);
            stopping = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error("reply did not reach actual settlement");
}
}

namespace {
// 用固定模型响应验证拒绝补库存后改用普通采掘的实际结算。
// 步骤：提出补库存确认 → 接收拒绝后扩展采掘工具 → 改用普通采掘并核对退出回执。
int verify_refused_collection(const std::filesystem::path& package, const std::filesystem::path& root) {
    using namespace dfy;
    using namespace model;
    using Json = nlohmann::json;
    std::atomic_bool receipt_seen = false;
    app::ApplicationRunConfig run;
    run.trace_directory = root;
    app::Application host({},root/"packages",root/"user",Backend{[&](const auto&, const Request& request) -> Response {
        const Response silent{{Role::assistant,R"({"internal":"fixed fixture settled"})"}};
        const auto call = [&](std::string_view name, Json args) -> Response {
            const auto tool = std::ranges::find_if(request.tools,[&](const auto& entry) { return entry.name.ends_with(name); });
            if (tool == request.tools.end()) throw std::runtime_error("required collection test tool was not visible");
            return {{Role::assistant,"",{{std::string(name),tool->name,args.dump()}}},FinishReason::tool_calls};
        };
        const auto& message = request.messages.back();
        // 即时拒绝走工具回执，已经接纳的动作走原工作结算。
        const auto verify = [&](const Json& result) {
            receipt_seen = result.value("reasonCode","") == "approval_required" && result.value("broken",-1) == 0;
            if (!receipt_seen) throw std::runtime_error("ordinary collection modified a refused candidate");
        };
        if (message.role == Role::tool) {
            if (message.tool_call_id == "_collect") {
                const auto result = Json::parse(message.content.substr(message.content.find('{')));
                if (result.value("status","") != "accepted") verify(result);
            }
            return silent;
        }
        const auto input = Json::parse(message.content);
        if (input.contains("workSettlement")) { verify(primary_receipt(input).at("result")); return silent; }
        if (input.at("trigger") == "skills.expanded") return call("_collect",{{"block","dirt"},{"count",1}});
        if (input.at("events").empty()) return silent;
        const auto& event = input.at("events").back();
        if (event.at("sequence") == 2)
            return call("_gather_ground",{{"item","dirt"},{"requiredStock",1},{"approval","game"}});
        if (event.at("sequence") == 4) return call("_skills",{{"categories",{"dig_break"}}});
        return silent;
    }},std::move(run));
    host.plugins().install(package);
    if (host.plugins().load("mc.companion")) throw std::runtime_error("could not load collection package");
    const auto id = host.open_companion("mc.companion");
    host.listen_companion(id);

    bool stopping = false;
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < deadline) {
        if (host.advance_plugins() == runtime::RunResult::failed) throw std::runtime_error("collection loop failed");
        if (stopping && host.companion_status()->closed) return 0;
        if (!stopping && receipt_seen && host.companion_status()->settled_event == 4) {
            host.stop_companion(id);
            stopping = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error("refused collection did not settle without a change");
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
        if (argc == 4 && std::wstring_view(argv[3]).starts_with(L"partner-execution-"))
            return verify_unavailable_partner(argv[1],argv[2],std::wstring_view(argv[3]).ends_with(L"-scenario"));
        if (argc == 4 && std::wstring_view(argv[3]).starts_with(L"partner-reply-"))
            return verify_reply_delivery(argv[1],argv[2],argv[3]);
        if (argc == 4 && std::wstring_view(argv[3]) == L"partner-refused-collection")
            return verify_refused_collection(argv[1],argv[2]);
        if (argc == 3) {
            verify_follow_windows();
            verify_collection_effect();
            verify_target_evidence();
            verify_opening_geometry();
            verify_route_and_dig_intent();
            verify_knowledge();
        }
        const bool interrupt = argc == 4 && std::wstring_view(argv[3]) == L"partner-interrupt";
        const bool suspend = argc == 4 && std::wstring_view(argv[3]).starts_with(L"partner-follow-collect");
        const bool recovery = argc == 4 && std::wstring_view(argv[3]) == L"partner-follow-recovery";
        const bool follow = suspend || recovery || (argc == 4 && std::wstring_view(argv[3]) == L"partner-follow");
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
                const bool reply = input.at("events").back().at("sequence") == 3;
                if (active.at("order") != 2) throw std::runtime_error("active motion lost the original order");
                if (recovery) {
                    const auto& tracking = input.at("following");
                    const auto& progress = active.at("progress");
                    if (tracking.at("work") != active.at("work") || tracking.at("segment") != 1 ||
                        tracking.at("status") != (reply ? "blocked" : "following") ||
                        progress.at("path") != (reply ? "unreachable" : "reachable") ||
                        progress.at("vertical") != (reply ? 5 : 0))
                        throw std::runtime_error("follow loss or recovery did not use the same active work");
                } else if (follow && (active.at("skill") != "follow" || active.at("progress").at("distance") != 4.5))
                    throw std::runtime_error("tracked progress missing from the active follow");
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
