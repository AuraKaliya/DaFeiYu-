#include "application.hpp"
#include "../../plugins/mc/src/follow_attention.hpp"
#include "../../plugins/mc/src/partner_effect.hpp"
#include "../../plugins/mc/src/partner_knowledge.hpp"
#include "../../plugins/mc/src/partner_task.hpp"
#include <dafeiyu/companion_abi_v2.h>
#include <dafeiyu/agent_abi_v2.h>
#include <dafeiyu/tool_abi_v2.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <semaphore>
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
    for (std::uint64_t at = 11500; at <= 12500; at += 500)
        if (sample(at,2.8,2.8,"in_range","reachable") != FollowSignal::none)
            throw std::runtime_error("range alone falsely recovered from a height obstacle");
    for (std::uint64_t at = 13000; at <= 14000; at += 500)
        if (sample(at,2.8,1,"in_range","reachable") != (at == 14000 ? FollowSignal::recovered : FollowSignal::none))
            throw std::runtime_error("the same follow episode did not recover after the height changed");

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
    if (!verified_action(result,"collect","") || verified_action(result,"collect","dirt"))
        throw std::runtime_error("unattributed inventory gain advanced a pickup dependency");
    result["pickups"] = {{"selfCollected",{{"dirt",1}}}};
    if (!verified_action(result,"collect","dirt") ||
        verified_action(result,"collect","grass_block")) throw std::runtime_error("grass drop was not dirt");
    result["inventoryDelta"].clear();
    if (!verified_action(result,"collect","") || verified_action(result,"collect","dirt"))
        throw std::runtime_error("block removal and material pickup were conflated");
    result["status"] = "blocked";
    if (verified_action(result,"collect","")) throw std::runtime_error("partial collect may not advance a dependency");
    result["status"] = "done";
    result["progress"] = {{"block","oak_door"},{"targetCell",{{"x",3},{"y",-60},{"z",2}}}};
    result["changes"] = {{{"x",3},{"y",-59},{"z",2},{"before","oak_door"},{"after","air"}},
        {{"x",3},{"y",-60},{"z",2},{"before","oak_door"},{"after","stone"}}};
    if (verified_action(result,"collect","")) throw std::runtime_error("cleared upper half certified the wrong target");
    result["changes"][1]["after"] = "air";
    if (!verified_action(result,"collect","")) throw std::runtime_error("delayed target change was missed");
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

// 核对原话的多开口选择在自身复查后才生成放置范围。
// 步骤：登记两个竖向开口 → 拦截越过确定指向或缺少读回的选择 → 绑定已核实的四格。
void verify_opening_selection() {
    using Json = nlohmann::json;
    dfy::mc::PartnerTask task;
    const Json position{{"x",4},{"y",-60},{"z",-6}};
    const Json anchor{{"dimension","overworld"},{"position",position}};
    const Json gaps = Json::array({
        {{"id","near"},{"x",3},{"y",-60},{"z",-3},{"width",1},{"height",2},{"axis","x"}},
        {{"id","far"},{"x",5},{"y",-60},{"z",-3},{"width",1},{"height",2},{"axis","x"}}});
    task.record({{"skill","observe"},{"result",{{"status","done"},{"anchor",{{"message",2}}},
        {"sampledAtMs",10},{"playerFocus",{{"choice","near"},{"candidates",gaps}}}}}});
    Json args{{"goal","补原墙的两处开口"},{"next","player"},{"target",{{"message",2},{"candidates",{"far"}}}}};
    if (task.update(args,anchor,0,&anchor).at("reason") != "opening_selection_unverified")
        throw std::runtime_error("opening selection dropped the original pointed candidate");
    args["target"]["candidates"] = {"near","far"};
    const auto pending = task.update(args,anchor,0,&anchor);
    if (pending.at("reason") != "current_observation_required" || pending.at("cells").size() != 4 || task.revision())
        throw std::runtime_error("multiple openings did not require a verified cell scope");
    auto verified = pending.at("cells");
    for (auto& cell : verified) {
        cell["block"] = "air";
        cell["atSelf"] = true;
    }
    const Json sample{{"views",Json::array({{{"source","self"},{"status","sampled"},{"actor",{{"position",position}}}}})},
        {"verified",verified}};
    task.record({{"skill","observe"},{"result",{{"status","done"},{"observation",
        {{"id","original-voids"},{"dimension","overworld"},{"samples",Json::array({sample})}}}}}});
    args["target"]["observation"] = "original-voids";
    if (task.update(args,anchor,0,&anchor).at("status") != "updated" || task.target_cells() != pending.at("cells") ||
        task.context().at("targetObservation").at("playerFocus").at("candidates") != gaps)
        throw std::runtime_error("verified original openings did not become one bounded placement target");
}

// 只把角色脚下已加载的格子加入自身取证，并核对精确材质与维度。
// 步骤：提交同帧各来源 → 核对负坐标对应格和材质 → 拒绝其他玩家和其他维度的读回。
void verify_below_feet_evidence() {
    using Json = nlohmann::json;
    dfy::mc::PartnerTask task;
    const Json self{{"x",1},{"y",-61},{"z",-6},{"block","grass_block"}};
    const Json player{{"x",2},{"y",-61},{"z",-5},{"block","dirt"}};
    const Json views = Json::array({{{"source","self"},{"status","sampled"},{"belowFeet",self},
        {"actor",{{"position",{{"x",1.25},{"y",-60},{"z",-5.41}}}}}},
        {{"source","player"},{"status","sampled"},{"belowFeet",player}}});
    task.record({{"skill","observe"},{"result",{{"status","done"},{"observation",
        {{"id","underfoot"},{"dimension","overworld"},{"samples",Json::array({{{"views",views}}})}}}}}});
    auto candidate = self;
    candidate["observation"] = "underfoot";
    const Json scene{{"dimension","overworld"}};
    if (!task.dig_observation_issue(candidate,scene).empty() ||
        task.dig_observation_issue(candidate,{{"dimension","the_nether"}}) != "observation_stale")
        throw std::runtime_error("loaded below-feet evidence lost its coordinate or world binding");
    candidate["block"] = "dirt";
    if (task.dig_observation_issue(candidate,scene) != "observation_block_mismatch")
        throw std::runtime_error("another material was accepted under the same observation");
    candidate = player;
    candidate["observation"] = "underfoot";
    if (task.dig_observation_issue(candidate,scene) != "observation_cell_unverified")
        throw std::runtime_error("the player's below-feet voxel was accepted as self evidence");
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
    const bool direct_place = variant.starts_with(L"partner-revision-direct-place");
    const bool stale_place = variant.ends_with(L"-direct-place-stale");
    const bool rejoin = variant.ends_with(L"-direct-rejoin");
    const bool notes = variant.ends_with(L"-notes");
    const bool approval = variant.ends_with(L"-approval");
    const bool goal = variant == L"partner-revision-goal" || direct_place;
    const bool correction = variant.starts_with(L"partner-revision-correction");
    const bool late_exit = variant == L"partner-revision-correction-late";
    const bool correction_lost = variant == L"partner-revision-correction-lost" || late_exit;
    const bool navigation = variant.starts_with(L"partner-revision-navigation");
    const bool restart_route = variant == L"partner-revision-navigation-restart";
    const bool mixed_route = variant == L"partner-revision-navigation-mixed";
    const bool repair_observe = variant == L"partner-revision-repair-observe";
    const bool repair_plan = variant == L"partner-revision-repair-plan";
    const bool repair_plan_observe = variant == L"partner-revision-repair-plan-observe";
    const bool observation_batch = variant == L"partner-revision-observation-batch";
    const bool response_timing = variant == L"partner-revision-response-timing";
    const bool layered = variant.starts_with(L"partner-revision-layered");
    const bool layered_stock = variant == L"partner-revision-layered-stock";
    const bool layered_blocked = variant == L"partner-revision-layered-blocked";
    const bool attention = variant == L"partner-revision-attention";
    const bool question_batch = variant == L"partner-revision-question-batch";
    const bool question_pair = variant.starts_with(L"partner-revision-question-pair");
    const bool failed_question = question_pair && variant.ends_with(L"-failed");
    const bool failed_body = variant.ends_with(L"-body-failed");
    const bool question_scope = question_batch || variant == L"partner-revision-question-scope";
    const bool opening_answer = variant == L"partner-revision-opening-answer";
    const bool air_priority = variant == L"partner-revision-air-priority";
    const bool air_budget = air_priority || variant == L"partner-revision-air-budget";
    const bool continuation_budget = air_budget || variant == L"partner-revision-continuation-budget";
    const bool question_expiry = variant == L"partner-revision-question-expiry";
    const bool observe_dig_scope = variant.starts_with(L"partner-revision-observe-dig-scope");
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
    std::uint64_t second_item = 0;
    unsigned budget_plans = 0;
    std::string follow_id, old_work, plan_tool;
    std::uint64_t original_admission = 0;
    app::ApplicationRunConfig run;
    run.trace_directory = root;
    Config model_config;
    if (observation_batch || response_timing) {
        model_config.provider = "deepseek";
        model_config.model = "fixed-context";
        model_config.thinking = response_timing;
    }
    app::Application host(model_config,root/"packages",root/"user",Backend{[&](const auto&, const Request& request) -> Response {
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
        if (response_timing) {
            if (last.role == Role::tool) {
                if (last.tool_call_id == "timing-first-reply") {
                    const Json steps = Json::array({
                        {{"id","corner"},{"skill","move"},{"arguments",{{"x",2},{"y",-60},{"z",1}}}},
                        {{"id","door"},{"skill","move"},{"after",{"corner"}},{"arguments",{{"x",4},{"y",-60},{"z",1}}}},
                    });
                    return call("_queue_actions","timing-plan",{{"operation","plan"},{"steps",steps}});
                }
                if (last.tool_call_id != "timing-plan") throw std::runtime_error("unexpected timing tool result");
                return {{Role::assistant,R"({"internal":"计划已接纳"})"}};
            }

            const auto input = Json::parse(last.content);
            if (input.at("trigger") == "plan.finished") {
                checked = true;
                return {{Role::assistant,"两段都走完了。"}};
            }
            const auto& event = input.at("events").back();
            const auto order = event.at("sequence");
            if (order == 1) return {{Role::assistant,R"({"internal":"ready"})"}};
            if (order == 2) return call("_reply","timing-first-reply",{{"text","好，我现在过去。"}});

            const auto& motion = input.at("activeMotion");
            if (order != 3 || event.at("text") != "还在走吗？" || motion.at("order") != 2 || motion.at("status") != "running")
                throw std::runtime_error("the live question did not see the original running work");
            // 保持固定模型响应在途，直到宿主收到原动作的实际抵达回执。
            std::ofstream(root/"timing-chat-open").put('1');
            const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < deadline) {
                std::ifstream trace(root/"trace"/"events.jsonl");
                for (std::string line; std::getline(trace,line);) {
                    const auto row = Json::parse(line,nullptr,false);
                    if (row.is_object() && row.value("category","") == "mc.partner.action" &&
                        row.at("body").at("bridgeWork") == motion.at("work") && row.at("body").at("result").value("arrived",false))
                        return {{Role::assistant,"还在走，接着去门边。"}};
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            throw std::runtime_error("the original work stopped progressing while the model answered");
        }
        if (observation_batch) {
            const Json args{{"mode","scene"},{"sources",{"self","player","message"}},{"message",2},{"radius",1}};
            if (last.role == Role::tool) {
                const auto result = Json::parse(last.content.substr(last.content.find('{')));
                if (last.tool_call_id == "duplicate-scene") {
                    const auto first = std::ranges::find(request.messages,"first-scene",&Message::tool_call_id);
                    if (first == request.messages.end()) throw std::runtime_error("the original observation is missing from the batch");
                    const auto observation = Json::parse(first->content.substr(first->content.find('{'))).at("observation");
                    const auto& sample = observation.at("samples").front();
                    const auto& views = sample.at("views");
                    const auto& anchor = views.at(2).at("anchor");
                    if (anchor.at("position") != Json{{"x",0},{"y",-60},{"z",10}} ||
                        anchor.at("positionCell") != Json{{"x",-1},{"y",-61},{"z",9}} ||
                        !anchor.at("positionCell").at("y").is_number_integer() ||
                        anchor.at("facing") != Json{{"yaw",0.12},{"pitch",-0.99}})
                        throw std::runtime_error("model observation lost the original block at a rounding boundary");
                    if (result != Json{{"status","done"},{"observationWork",observation.at("id")}} ||
                        sample.at("sampledAtMs") != 142 || views.at(0).at("source") != "self" ||
                        views.at(0).at("actor").at("position").at("x") != -1.5 || views.at(1).at("source") != "player" ||
                        views.at(1).at("status") != "unavailable" || views.at(2).at("source") != "message" ||
                        views.at(2).at("message") != 2 || views.at(2).at("anchor").at("sampledAtMs") != 42)
                        throw std::runtime_error("batch did not retain one observation with its original sources and time");
                    return call("_observe","refresh-scene",args);
                }
                if (last.tool_call_id != "refresh-scene") throw std::runtime_error("unexpected observation turn");
                const auto& sample = result.at("observation").at("samples").front();
                const auto& views = sample.at("views");
                if (sample.at("sampledAtMs") != 242 || views.at(1).at("status") != "sampled" ||
                    views.at(1).at("actor").at("position").at("x") != 2.5 || views.at(2).at("anchor").at("sampledAtMs") != 42)
                    throw std::runtime_error("explicit refresh reused a previous batch or lost the original anchor");
                const Json step{{"id","meet"},{"skill","move"},{"arguments",{{"x",2},{"y",-60},{"z",1}}}};
                return call("_queue_actions","meet-player",{{"operation","plan"},{"steps",Json::array({step})}});
            }
            const auto input = Json::parse(last.content);
            if (input.at("trigger") == "plan.finished") {
                if (input.at("planSettlements").at(0).at("nodes").at(0).at("result").at("arrived") != true)
                    throw std::runtime_error("the verified plan did not proceed to actual arrival");
                checked = true;
                return {{Role::assistant,R"({"internal":"plan exit checked"})"}};
            }
            if (input.at("events").back().at("sequence") == 1) return {{Role::assistant,R"({"internal":"ready"})"}};
            auto batch = call("_observe","first-scene",args);
            batch.message.tool_calls.push_back(call("_observe","duplicate-scene",args).message.tool_calls.front());
            return batch;
        }
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
                    input.at("followAttention").contains("reason") || input.at("followAttention").at("previousReason") != "unreachable_height" ||
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
                if (last.tool_call_id == "survey") {
                    old_work = result.at("observation").at("id").get<std::string>();
                    return call("_task_update","bind-stale",{{"goal","垫两级站位"},{"next","player"},{"target",{{"message",3},
                        {"cells",pads},{"observation","older-observation"}}}});
                }
                if (last.tool_call_id == "bind-stale") {
                    if (result.at("reason") != "current_observation_required") throw std::runtime_error("stale pad binding was admitted");
                    return call("_task_update","bind",{{"goal","垫两级站位"},{"next","player"},{"target",{{"message",3},{"cells",pads},
                        {"observation",old_work}}}});
                }
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
                const std::size_t first_failed = layered_stock ? 2 : layered_blocked ? 1 : 4;
                if (checked || plan.at("origin").at("sequence") != 3 ||
                    plan.at("status") != (first_failed < 4 ? "blocked" : "done") || layers.size() != 4 ||
                    input.at("following").at("work") != follow_id)
                    throw std::runtime_error("layered plan did not settle all dependencies");
                for (std::size_t i = 0; i < layers.size(); ++i) {
                    const auto& result = layers.at(i).at("nodes").at(0).at("result");
                    if (result.at("status") != (i < first_failed ? "done" : i == first_failed ? "blocked" : "skipped") ||
                        (i < first_failed && (i%2 == 0 ? result.at("placed") != true : result.at("arrived") != true)))
                        throw std::runtime_error("layer lacked placement, arrival, or skip evidence");
                }
                if (layered_stock && layers.at(2).at("nodes").at(0).at("result").at("reasonCode") != "material_missing")
                    throw std::runtime_error("a blocked placement lost its specific cause");
                checked = true;
                return {{Role::assistant,layered_stock ? "第一块垫好了，背包没有第二块了。" :
                    layered_blocked ? "第一块垫好了，但我站不上去。" : R"({"internal":"plan exit checked"})"}};
            }
            if (!input.at("events").empty()) {
                const auto& sequence = input.at("events").back().at("sequence");
                if (sequence == 2) return call("_follow","start-follow",Json::object());
                if (sequence == 3) {
                    follow_id = input.at("following").at("work").get<std::string>();
                    if (input.at("activeMotion").at("work") != follow_id || input.at("activeMotion").at("status") != "running")
                        throw std::runtime_error("layered plan did not start during the original active follow");
                    return call("_task_update","register",{{"operation","start_task"},{"kind","work"},{"message",3},{"goal","垫两级站位"}});
                }
            }
            return {{Role::assistant,R"({"internal":"ready"})"}};
        }
        if (pointed_craft) {
            const Json continuation{{"item","planks"},{"count",4}};
            const std::string question = "你指哪块原木？";
            const std::string final = no_pickup ? "木头敲下来了，我还没捡到。" : "一根原木做成了四块木板。";
            if (last.role == Role::tool) {
                if (last.tool_call_id == "plan-focus-denied" || last.tool_call_id == "restricted-plan") {
                    if (!last.content.starts_with("status=rejected; effect=not_applied"))
                        throw std::runtime_error("plan-only focus or restricted plan was accepted");
                    const auto detail = Json::parse(last.content.substr(last.content.find('{')));
                    if (last.tool_call_id == "plan-focus-denied" && (detail.at("reason") != "outside_enum" ||
                        detail.at("path") != "/steps/0/arguments/focus" || detail.at("actual") != "objects" ||
                        !detail.at("expected").is_array() || std::ranges::find(detail.at("expected"),"objects") != detail.at("expected").end()))
                        throw std::runtime_error("invalid plan focus lost its field or allowed values");
                    if (last.tool_call_id == "restricted-plan" && (detail.at("stage") != "scope" || detail.at("reason") != "not_granted"))
                        throw std::runtime_error("out-of-scope tool reported an argument failure");
                    if (last.tool_call_id == "restricted-plan") return call("_reply","final-line",{{"text",final}});
                    return call("_observe","first-focus",{{"mode","pointed"},{"message",2},{"focus","objects"}});
                }
                const auto result = Json::parse(last.content.substr(last.content.find('{')));
                if (last.tool_call_id == "list-skills" || last.tool_call_id == "expand-skills" || last.tool_call_id == "repeat-skills") {
                    const auto& directory = result.at("categories");
                    const auto craft = std::ranges::find_if(directory,[](const auto& item) { return item.at("category") == "craft_process"; });
                    const auto& contracts = result.at("contracts");
                    const auto craft_entry = std::ranges::find_if(contracts,[](const auto& item) { return item.at("skill") == "craft"; });
                    const auto observe = std::ranges::find_if(contracts,[](const auto& item) { return item.at("skill") == "observe"; });
                    const auto extract = [&](std::string_view skill) -> const Json& {
                        const auto entry = std::ranges::find_if(contracts,[&](const auto& row) {
                            return row.at("skill").template get_ref<const std::string&>() == skill;
                        });
                        if (entry == contracts.end()) throw std::runtime_error("missing capability "+std::string(skill));
                        return *entry;
                    };
                    if (result.at("status") != "listed" || result.at("wake") != false || craft == directory.end() ||
                        craft->at("available") != true || !craft->at("tools").empty() || craft->at("planNodes") != Json::array({"craft"}) ||
                        craft_entry == contracts.end() || observe == contracts.end() || craft_entry->at("implemented") != true ||
                        craft_entry->contains("direct") || craft_entry->at("plan").at("visible") != true ||
                        craft_entry->at("plan").at("conditions").at("status") != "check_on_execution" ||
                        observe->at("direct").at("visible") != true || observe->at("plan").at("visible") != true)
                        throw std::runtime_error("directory conflicted with the existing plan or tool scope");
                    const auto dig = std::ranges::find_if(directory,[](const auto& row) { return row.at("category") == "dig_break"; });
                    const auto& move = extract("move"), &collect = extract("collect"), &place = extract("place");
                    if (dig == directory.end() || dig->at("planNodes") != Json::array({"dig_block","gather_ground"}) ||
                        move.contains("direct") || move.at("sequence").at("visible") != true ||
                        collect.contains("plan") || collect.at("direct").at("visible") != false || collect.at("sequence").at("visible") != false ||
                        place.at("plan").at("conditions").at("status") != "target_unbound" ||
                        extract("stop_motion").at("direct").at("visible") != false)
                        throw std::runtime_error("directory confused an entry point or a known execution prerequisite");

                    const auto& entry = craft_entry->at("plan");
                    const Json categories{{"categories",Json::array({"observation","craft_process"})}};
                    const auto& procedure = result.at("procedures").front();
                    if (procedure.at("name") != "就近取材" || procedure.at("category") != "dig_break" || procedure.contains("guidance"))
                        throw std::runtime_error("the light directory did not disclose the name of the gathering skill");
                    if (last.tool_call_id == "list-skills") {
                        if (entry.contains("arguments")) throw std::runtime_error("default list expanded plan details");
                        return call("_skills","expand-skills",categories);
                    }
                    const auto tool = std::ranges::find(request.tools,plan_tool,&ToolDefinition::name);
                    if (tool == request.tools.end()) throw std::runtime_error("plan-only expansion removed the queue tool");
                    const auto schema = Json::parse(tool->parameters);
                    const auto& description = schema.at("properties").at("steps").at("items").at("properties")
                        .at("skill").at("description").get_ref<const std::string&>();
                    if (description.find(entry.at("arguments").get_ref<const std::string&>()) == description.npos ||
                        description.find(observe->at("plan").at("arguments").get_ref<const std::string&>()) == description.npos)
                        throw std::runtime_error("catalog parameters differed from the actual tool snapshot");
                    if (last.tool_call_id == "expand-skills") return call("_skills","repeat-skills",categories);
                    return call("_queue_actions","plan-focus-denied",{{"operation","plan"},{"steps",Json::array({
                        {{"skill","observe"},{"arguments",{{"message",2},{"focus","objects"}}}}})}});
                }
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
                const auto& successor = layers.at(1).at("nodes").at(0);
                const auto& craft = successor.at("result");
                if (checked || input.contains("pendingQuestion") || request.tools.size() != 1 ||
                    !request.tools.front().name.ends_with("_reply") || layers.size() != 2 ||
                    nodes.at(1).at("after") != Json::array({"gather_log"}) || nodes.at(0).at("requiresPickup") != "oak_log" ||
                    dig.at("changes").at(0).at("before") != "oak_log")
                    throw std::runtime_error("compound goal or original pickup work was lost at settlement");
                if (no_pickup ? dig.at("reasonCode") != "pickup_unverified" || dig.at("inventoryDelta").at("oak_log") != 1 ||
                    dig.at("pickups").at("otherCollected").at("oak_log") != 1 || craft.at("reason") != "prerequisite_failed" :
                    dig.at("inventoryDelta").at("oak_log") != 1 || craft.at("status") != "done" ||
                    craft.at("craft").at("consumed") != 1 || craft.at("craft").at("produced") != 4 ||
                    craft.at("inventoryDelta") != Json{{"oak_log",-1},{"oak_planks",4}} || craft.at("count") != 4)
                    throw std::runtime_error("crafting advanced without pickup or lost its requested product");
                return {{Role::assistant,"",{{"restricted-plan",plan_tool,R"({"operation":"plan","steps":[{"skill":"observe","arguments":{}}]})"}}},
                    FinishReason::tool_calls};
            }
            const auto& event = input.at("events").back();
            const auto sequence = event.at("sequence").get<std::uint64_t>();
            if (sequence == 1) return {{Role::assistant,R"({"internal":"ready"})"}};
            if (sequence == 2) {
                const auto tool = std::ranges::find_if(request.tools,[](const auto& item) { return item.name.ends_with("_queue_actions"); });
                if (tool == request.tools.end() || std::ranges::any_of(request.tools,[](const auto& item) { return item.name.ends_with("_craft"); }))
                    throw std::runtime_error("default scope did not expose craft exclusively as a plan node");
                plan_tool = tool->name;
                return call("_skills","list-skills",Json::object());
            }
            if (sequence == 3 || sequence == 4) {
                const auto& pending = input.at("pendingQuestion");
                if (pending.at("question") != question || pending.at("afterPickup") != continuation ||
                    pending.at("originalText") != "这里有个原木，敲掉它，然后做成4个木板")
                    throw std::runtime_error("compound goal did not persist through ordinary chat");
                if (sequence == 3) return call("_reply","heard-player",{{"text","听得到。"}});
            }
            return call("_observe","answer-focus",{{"mode","pointed"},{"message",sequence},{"focus","objects"}});
        }
        if (observe_dig_scope) {
            const bool plan = variant.ends_with(L"-plan");
            const bool evidence = variant.find(L"-evidence") != variant.npos;
            const Json cell{{"x",1},{"y",-59},{"z",2}};
            const Json unloaded{{"x",1},{"y",-59},{"z",3}};
            if (last.role == Role::tool) {
                const auto& id = last.tool_call_id;
                if (id != "look-at-other-block" && id != "stale-sample" && id != "invalid-cell")
                    throw std::runtime_error("the unrelated look prevented the dig from starting: " + last.content);
                auto args = cell;
                args["block"] = "dirt";
                const char* call_id = "dig-in-scope";
                if (evidence) {
                    const auto result = Json::parse(last.content.substr(last.content.find('{')));
                    if (id == "look-at-other-block") {
                        old_work = result.at("observation").at("id").get<std::string>();
                        args["observation"] = "older-observation";
                        call_id = "stale-sample";
                    } else if (id == "stale-sample") {
                        if (result.at(plan ? "reason" : "reasonCode") != "observation_stale")
                            throw std::runtime_error("a stale observation reference was admitted");
                        if (plan) args["z"] = unloaded.at("z");
                        else args["block"] = "grass_block";
                        call_id = "invalid-cell";
                    } else if (result.at(plan ? "reason" : "reasonCode") != (plan ? "observation_cell_unverified" : "observation_block_mismatch")) {
                        throw std::runtime_error("a cell that did not match the loaded observation was admitted");
                    }
                    if (id != "look-at-other-block") args["observation"] = old_work;
                }
                if (!plan) return call("_dig_block",call_id,args);
                Json node{{"id","dig"},{"skill","dig_block"},{"arguments",args}};
                if (evidence) node["pickup"] = "dirt";
                return call("_queue_actions",call_id,{{"operation","plan"},{"steps",Json::array({std::move(node)})}});
            }

            const auto input = Json::parse(last.content);
            if (input.at("trigger") == (plan ? "plan.finished" : "motion.finished")) {
                const auto& receipt = plan ? input.at("planSettlements").at(0).at("nodes").at(0) :
                    input.at("workSettlement").at("receipts").at(0);
                if (receipt.at("result").at("inventoryDelta").value("dirt",0) != 1)
                    throw std::runtime_error("dig did not collect the requested material");
                checked = true;
                return {{Role::assistant,R"({"internal":"plan exit checked"})"}};
            }
            if (input.at("events").back().at("sequence") == 2) {
                if (evidence) return call("_observe","look-at-other-block",{{"mode","scene"},{"sources",{"self"}},
                    {"verify",Json::array({cell,unloaded})}});
                return call("_observe","look-at-other-block",{{"mode","pointed"},{"message",2},{"focus","objects"}});
            }
            return {{Role::assistant,R"({"internal":"ready"})"}};
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
        if (continuation_budget) {
            const Response quiet{{Role::assistant,R"({"internal":"ready"})"}};
            if (last.role == Role::tool) return quiet;
            const auto input = Json::parse(last.content);
            if (air_budget && input.at("trigger") == "vitals.attention") {
                if (budget_plans != 5 || ++attention_turns != 1 || input.contains("continuationBudgetExhausted") ||
                    input.at("airNotices").back().at("sequence") != 1 || request.tools.size() != 1 ||
                    !request.tools.front().name.ends_with("_observe")) throw std::runtime_error("independent air budget unavailable");
                std::ofstream(root/"air-budget-observed").put('1');
                if (air_priority) return call("_observe","slow-air",{{"mode","vitals"}});
                return {{Role::assistant,"独立观察已核对。"}};
            }
            if (air_budget && !input.at("events").empty() && input.at("events").back().at("sequence") == 3) {
                const auto& notice = input.at("airNotices").back();
                if (attention_turns != 1 || notice.at("sequence") != 4 || notice.at("missedBefore") != 2 ||
                    input.at("replyOrigin").at("sourceMessage") != 3) throw std::runtime_error("chat lost its turn or missed air boundaries");
                if (air_priority) {
                    const auto& prior = input.at("interruptedAirCheck");
                    if (prior.at("noticeSequence") != 1 || prior.at("yieldRequestedBy") != 3 ||
                        prior.at("lastObservation").at("status") != "interrupted" || prior.at("outcome") != "yielded_to_player")
                        throw std::runtime_error("cancelled air check lost its actual exit or source");
                }
                checked = true;
                return {{Role::assistant,"我收到你的问题了，刚才两条空气变化的细节还需要复查。"}};
            }
            if (input.at("trigger") == "plan.finished") {
                const auto& result = input.at("planSettlements").at(0).at("nodes").at(0).at("result");
                if (result.at("status") != "done") throw std::runtime_error("continuation lost an actual settlement");
                if (++budget_plans == 5) {
                    if (input.at("continuationBudgetExhausted") != true || request.tools.size() != 1 ||
                        !request.tools.front().name.ends_with("_reply")) throw std::runtime_error("settlements could keep creating new work");
                    if (air_budget) std::ofstream(root/"air-budget-ready").put('1');
                    else checked = true;
                    return {{Role::assistant,R"({"internal":"plan exit checked"})"}};
                }
                if (input.contains("continuationBudgetExhausted")) throw std::runtime_error("continuation stopped before the last allowed step");
            } else if (input.at("events").back().at("sequence") != 2) return quiet;
            const Json observation{{"id","look"},{"skill","observe"},{"arguments",Json::object()}};
            return call("_queue_actions","plan-observation",{{"operation","plan"},{"steps",Json::array({observation})}});
        }
        if (opening_answer) {
            const Json scope{{"skill","place"}};
            const Json cells = Json::array({{{"x",2},{"y",-60},{"z",-5}},{{"x",2},{"y",-59},{"z",-5}},
                {{"x",4},{"y",-60},{"z",-5}},{{"x",4},{"y",-59},{"z",-5}}});
            if (last.role == Role::tool) {
                const auto result = Json::parse(last.content.substr(last.content.find('{')));
                const auto& id = last.tool_call_id;
                if (id == "original-openings") return call("_ask_player","ask-openings",{{"question","补哪些缺口？"},{"scope",scope}});
                if (id == "ask-openings") placement_item = result.at("item").get<std::uint64_t>();
                if (id == "wrong-question") {
                    if (result.at("reason") != "question_resolution_mismatch") throw std::runtime_error("wrong question item accepted");
                    return call("_observe","second-view",{{"sources",{"message"}},{"message",2},{"focus","openings"}});
                }
                if (id == "second-view") {
                    if (result.at("playerFocus").at("candidates").size() != 1) throw std::runtime_error("changed openings were not observed");
                    return call("_observe","verify-openings",{{"sources",{"self"}},{"verify",cells}});
                }
                if (id == "verify-openings") return call("_task_update","answer-openings",{{"goal","补齐原墙的两处开口"},
                    {"next","continue"},{"resolves",placement_item},{"target",{{"message",2},{"candidates",{"left","right"}},
                        {"observation",result.at("observation").at("id")}}}});
                if (id == "answer-openings") {
                    const auto& answer = result.at("resolvedQuestion");
                    if (answer.at("item") != placement_item || answer.at("sourceMessage") != 2 || answer.at("answerMessage") != 3 ||
                        answer.at("scope") != scope || answer.at("answer") != "两边都补上吧" || result.at("task").at("target").at("cells") != cells)
                        throw std::runtime_error("natural clarification lost the original source or the chosen scope");
                    return {{Role::assistant,R"({"internal":"clarification checked"})"}};
                }
            } else {
                const auto input = Json::parse(last.content);
                if (input.at("trigger") == "skills.expanded") {
                    const auto& clarification = input.at("task").at("clarification");
                    if (clarification.at("origin").at("sequence") != 2 || clarification.at("answerMessage") != 3)
                        throw std::runtime_error("clarification was lost before the capability expanded");
                    const Json fill{{"id","fill"},{"skill","fill_target"},{"arguments",{{"block","dirt"}}}};
                    return call("_queue_actions","fill-openings",{{"operation","plan"},{"steps",Json::array({fill})}});
                }
                if (input.at("trigger") == "plan.finished") {
                    const auto& plan = input.at("taskPlan");
                    const auto& result = input.at("planSettlements").at(0).at("nodes").at(0).at("result");
                    if (input.contains("pendingQuestion") || result.at("changes").size() != cells.size() ||
                        plan.at("origin").at("text") != "把墙上的口补一下" || plan.at("clarification").at("item") != placement_item ||
                        plan.at("clarification").at("answer") != "两边都补上吧")
                        throw std::runtime_error("the scope did not settle both openings");
                    checked = true;
                    return {{Role::assistant,R"({"internal":"plan exit checked"})"}};
                }
                if (!input.at("events").empty()) {
                    const auto sequence = input.at("events").back().at("sequence");
                    if (sequence == 2) return call("_observe","original-openings",{{"sources",{"message"}},{"message",2},{"focus","openings"}});
                    if (sequence == 3) {
                        const auto& pending = input.at("pendingQuestion");
                        if (pending.at("item") != placement_item || pending.at("sourceMessage") != 2 ||
                            pending.at("openingObservation").at("playerFocus").at("candidates").size() != 2)
                            throw std::runtime_error("the answer did not receive original opening evidence");
                        return call("_task_update","wrong-question",{{"goal","补墙"},{"next","continue"},{"resolves",placement_item+1}});
                    }
                }
            }
            return {{Role::assistant,R"({"internal":"waiting"})"}};
        }
        if (question_pair) {
            const Response quiet{{Role::assistant,R"({"internal":"waiting"})"}};
            const auto ask = [&](bool second) {
                return call("_ask_player",second ? "ask-second" : "ask-first",
                    {{"question",std::string(215,second ? 'B' : 'A')},{"sourceMessage",second ? 3 : 2},
                     {"scope",{{"skill","collect"},{"block",second ? "coarse_dirt" : "grass_block"}}},{"options",{"可以","不行"}}});
            };
            const auto plan = [&](bool answered) {
                Json steps = Json::array();
                for (const auto block : {"grass_block","coarse_dirt","dirt"}) {
                    if (answered && std::string_view(block) == "dirt") continue;
                    steps.push_back({{"id",block},{"skill","dig_block"},{"arguments",{{"block",block},{"x",1},{"y",-61},
                        {"z",static_cast<int>(steps.size())+2}}}});
                }
                return call("_queue_actions","check-scopes",{{"operation","plan"},{"steps",steps}});
            };
            if (last.role == Role::tool) {
                const auto output = Json::parse(last.content.substr(last.content.find('{')));
                if (last.tool_call_id == "pair-task") return ask(false);
                if (last.tool_call_id == "new-task") { std::ofstream(root/"pair-revised").put('1'); return quiet; }
                if (last.tool_call_id == "ask-first") { placement_item = output.at("item"); return failed_question ? quiet : ask(true); }
                if (last.tool_call_id == "ask-second") {
                    second_item = output.at("item");
                    if (second_item == placement_item) throw std::runtime_error("independent questions shared an item");
                    return failed_question ? quiet : plan(false);
                }
                return quiet;
            }
            const auto input = Json::parse(last.content);
            if (failed_question) {
                if (input.contains("questionResult")) {
                    const auto& result = input.at("questionResult");
                    if (result.at("status") == "unavailable" && result.at("item") == second_item && result.at("delivery") == "not_sent" &&
                        result.at("bridgeWork").get<std::string>().ends_with(".number") != failed_body &&
                        input.at("pendingQuestion").at("item") == placement_item) {
                        std::ofstream(root/"pair-failed").put('1');
                        return quiet;
                    }
                    if (result.at("status") != "answered" || result.at("item") != placement_item || result.at("answer") != "可以")
                        throw std::runtime_error("an unseen item number locked the original question");
                    checked = true;
                    return {{Role::assistant,R"({"internal":"plan exit checked"})"}};
                }
                const auto& events = input.at("events");
                if (!events.empty() && events.back().at("sequence") == 2) return ask(false);
                if (!events.empty() && events.back().at("sequence") == 3) return ask(true);
                return quiet;
            }
            if (input.at("trigger") == "plan.finished") {
                const auto& nodes = input.at("planSettlements").at(0).at("nodes");
                if (nodes[0].at("result").at("item") != placement_item) throw std::runtime_error("first dependency lost");
                if (nodes.size() == 3) {
                    if (nodes[1].at("result").at("item") != second_item ||
                        nodes[2].at("result").at("inventoryDelta").at("dirt") != 1) throw std::runtime_error("unrelated work blocked");
                    std::ofstream(root/"pair-initial").put('1');
                } else {
                    if (nodes[1].at("result").at("inventoryDelta").at("coarse_dirt") != 1)
                        throw std::runtime_error("answered dependency still blocked");
                    std::ofstream(root/"pair-released").put('1');
                }
                return quiet;
            }
            const auto seq = input.at("events").back().at("sequence").get<int>();
            if (seq == 3) return call("_task_update","pair-task",{{"operation","start_task"},{"message",3},{"kind","work"},{"goal","分开备料"}});
            if (seq == 6) {
                const auto& both = input.at("pendingQuestions");
                if (both.size() != 2 || both[0].at("item") != placement_item || both[1].at("item") != second_item ||
                    input.contains("questionResult")) throw std::runtime_error("ambiguous or misaddressed answer closed an item");
                std::ofstream(root/"pair-invalid").put('1');
            }
            if (seq == 7 || seq == 10) {
                const bool second = seq == 7;
                const auto& answer = input.at("questionResult");
                if (answer.at("item") != (second ? second_item : placement_item) || answer.at("sourceMessage") != (second ? 3 : 2) ||
                    answer.at("originalText") != (second ? "砂土也备点" : "帮我备点草方块") || answer.at("answerMessage") != seq ||
                    answer.at("answer") != "可以" || answer.at("player") != input.at("player") ||
                    answer.at("taskRevision") != 1 || answer.at("taskRevision") != input.at("task").at("revision"))
                    throw std::runtime_error("answer lost its origin");
                if (second) {
                    if (input.at("pendingQuestion").at("item") != placement_item) throw std::runtime_error("first item disappeared");
                    return plan(true);
                }
                if (input.contains("pendingQuestion")) throw std::runtime_error("answered item still pending");
                std::ofstream(root/"pair-answered").put('1');
            }
            if (seq == 9) {
                if (input.at("pendingQuestion").at("item") != placement_item || input.contains("questionResult"))
                    throw std::runtime_error("duplicate answer released another item");
                std::ofstream(root/"pair-repeat").put('1');
            }
            if (seq == 11) return call("_task_update","new-task",{{"operation","start_task"},{"message",11},{"kind","work"},{"goal","等玩家回来"}});
            if (seq == 12) {
                if (input.at("task").at("revision") != 2 || input.contains("pendingQuestion") || input.contains("questionResult"))
                    throw std::runtime_error("late answer reopened a settled item");
                checked = true;
                return {{Role::assistant,R"({"internal":"plan exit checked"})"}};
            }
            return quiet;
        }
        if (question_scope) {
            const Json scope{{"skill","collect"},{"block","grass_block"}};
            const Json grass{{"x",1},{"y",-61},{"z",2},{"block","minecraft:grass_block"}};
            Json question{{"question","草方块也可以吗？"},{"scope",scope},{"options",{"可以用草方块","还是用泥土"}}};
            if (question_batch) question["sourceMessage"] = 2;
            if (last.role == Role::tool) {
                const auto result = Json::parse(last.content.substr(last.content.find('{')));
                if (last.tool_call_id == "wrong-source") {
                    if (result.at("status") != "invalid_source_message") throw std::runtime_error("non-player source registered a question");
                    return call("_ask_player","ask-about-grass",question);
                }
                if (last.tool_call_id == "ask-about-grass") {
                    placement_item = result.at("item").get<std::uint64_t>();
                    return call("_dig_block","wait-on-grass",grass);
                }
                if (last.tool_call_id != "wait-on-grass" || result.at("reason") != "waiting_for_player" || result.at("item") != placement_item)
                    throw std::runtime_error("a direct action entered the scoped question");
                auto dirt = grass;
                dirt["z"] = 3;
                dirt["block"] = "dirt";
                return call("_queue_actions","collect-while-waiting",{{"operation","plan"},{"steps",Json::array({
                    {{"id","grass"},{"skill","dig_block"},{"arguments",grass}},
                    {{"id","dirt"},{"skill","dig_block"},{"arguments",dirt}}})}});
            }
            const auto input = Json::parse(last.content);
            if (input.at("trigger") == "plan.finished") {
                const auto& nodes = input.at("planSettlements").at(0).at("nodes");
                const auto& pending = input.at("pendingQuestion");
                if (pending.at("item") != placement_item || pending.at("scope") != scope || pending.at("sourceMessage") != 2 ||
                    pending.at("originalText") != "挖一块泥土来垫脚" ||
                    nodes.at(0).at("result").at("reason") != "waiting_for_player" ||
                    nodes.at(1).at("result").at("inventoryDelta").at("dirt") != 1)
                    throw std::runtime_error("the question prevented independent gathering or changed identity");
                std::ofstream(root/"scoped-question-checked").put('1');
            } else if (const auto sequence = input.at("events").back().at("sequence"); sequence == (question_batch ? 3 : 2)) {
                if (question_batch) {
                    if (input.at("events").size() != 2) throw std::runtime_error("expected two independent player messages");
                    question["sourceMessage"] = 1;
                    return call("_ask_player","wrong-source",question);
                }
                return call("_ask_player","ask-about-grass",question);
            } else if (sequence == (question_batch ? 4 : 3)) {
                const auto& result = input.at("questionResult");
                if (input.contains("pendingQuestion") || result.at("item") != placement_item || result.at("scope") != scope ||
                    result.at("sourceMessage") != 2 || result.at("status") != "answered" ||
                    result.at("answerMessage") != sequence || result.at("answer") != "还是用泥土")
                    throw std::runtime_error("new clarification did not settle the original question");
                checked = true;
                return {{Role::assistant,R"({"internal":"plan exit checked"})"}};
            }
            return {{Role::assistant,R"({"internal":"waiting"})"}};
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
                if (input.contains("planRejection") || input.contains("previousPlanRejection") ||
                    !std::filesystem::exists(root/"observe-plan-settled"))
                    throw std::runtime_error("next message lost the settled observation plan");
                checked = true;
                return {{Role::assistant,"看过了，我还在。"}};
            }
            if (order != 2 && order != 3) return quiet;
            Json node{{"id","nearby"},{"skill","observe"},{"arguments",Json::object()}};
            if (order == 2) node["after"] = {"missing"};
            else if (input.contains("planRejection") || input.at("previousPlanRejection").at("order") != 2 ||
                input.at("previousPlanRejection").at("result").at("reason") != "invalid_dependency")
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
                if (mixed_route) {
                    steps.back()["after"] = {"approach"};
                    steps.push_back({{"id","leave"},{"skill","move"},{"arguments",route.at("waypoints").at(1)},{"after",{"circle"}}});
                }
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
                if (late_exit) {
                    std::ofstream(root/"awaiting-original-exit").put('1');
                    return {{Role::assistant,R"({"internal":"等待原工作退出"})"}};
                }
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
            if (stale_place && (id == "switch-placement-task" || id == "stale-placement")) {
                const auto result = Json::parse(last.content.substr(last.content.find('{')));
                if (id == "switch-placement-task") {
                    if (result.at("status") != "updated" || result.at("task").at("revision") != 2)
                        throw std::runtime_error("placement task was not superseded");
                    std::ofstream(root/"placement-waiting-checked").put('1');
                } else {
                    if (result.at("status") != "invalid_plan" || result.at("reason") != "target_unbound")
                        throw std::runtime_error("old answer authorized placement after task replacement");
                    checked = true;
                    return {{Role::assistant,R"({"internal":"plan exit checked"})"}};
                }
            }
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
        if (late_exit && input.at("trigger") == "task.ready") {
            const auto& work = input.at("hostWorkReceipts").at(0);
            const auto& handoff = input.at("taskHandoff");
            if (work.at("work") != old_work || work.at("accepted") != original_admission || work.at("revision") != 4 ||
                work.at("exited") >= work.at("effectReceipt") || work.at("effect").at("effect") != DFY_V2_TOOL_APPLIED ||
                work.at("effect").at("result").at("inventoryDelta").at("dirt") != 1 ||
                handoff.at("work") != old_work || handoff.at("action").at("bridgeExited") != true ||
                input.at("task").at("pendingCorrection").contains("unconfirmedWork"))
                throw std::runtime_error("late exit did not release the original correction with its verified effect");
            return call("_observe","observe-correction",{{"sources",{"message","self"}},{"message",2},{"focus","openings"},{"verify",wall}});
        }
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
                exit.at("status") != "exited" || exit.at("work") != old_work || exit.at("work") != action.at("bridgeWork") ||
                action.at("result").at("status") != "interrupted" ||
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
                const auto count = mixed_route ? 3u : 1u;
                const auto& route = layers.at(route_index).at("nodes").at(0).at("result").at("route");
                if (checked || plan.at("status") != "done" || layers.size() != count || route.at("laps") != 2 ||
                    route.at("checkpoints").size() != 9 || input.at("task").at("next") != "done" ||
                    (mixed_route ? first.at("result").at("arrived") != true ||
                        layers.at(2).at("nodes").at(0).at("result").at("arrived") != true :
                        request.tools.size() != 1 || !request.tools.front().name.ends_with("_reply")))
                    throw std::runtime_error("route plan did not report verified checkpoints and arrivals at settlement");
                checked = true;
                return {{Role::assistant,R"({"internal":"plan exit checked"})"}};
            }
            const auto& second = layers.at(1).at("nodes").at(0).at("result");
            const auto& result = first.at("result");
            if (correction_lost) {
                const auto& work = input.at("hostWorkReceipts").at(0);
                if (work.at("work") != first.at("work") || work.at("revision") != 2 || work.contains("exited") ||
                    work.at("effect").at("effect") != DFY_V2_TOOL_UNKNOWN || work.at("effect").at("bridgeExited") != false)
                    throw std::runtime_error("host reported a game exit while the original bridge work was unconfirmed");
                original_admission = work.at("accepted");
                old_work = first.at("work").get<std::string>();
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
                if (question.at("status") != "waiting" || input.contains("questionResult"))
                    throw std::runtime_error("the original placement question did not stay open");
                placement_item = question.at("item").get<std::uint64_t>();
                if (stale_place) return call("_task_update","switch-placement-task",{{"operation","start_task"},
                    {"message",3},{"kind","work"},{"goal","改清理另一处"}});
                return probe_placement("waiting-append",chosen);
            }
            if (direct_place && event.at("sequence") == 4) {
                const auto& answer = input.at("questionResult");
                if (answer.at("item") != placement_item || input.contains("pendingQuestion"))
                    throw std::runtime_error("placement reply did not retain the original item");
                if (stale_place) {
                    if (input.at("task").at("revision") != 2 || answer.at("status") != "superseded" ||
                        answer.contains("answerMessage") || answer.contains("placementCells"))
                        throw std::runtime_error("late reply resolved a superseded placement question");
                    return call("_queue_actions","stale-placement",{{"operation","plan"},{"steps",Json::array({
                        {{"id","old_cell"},{"skill","place"},{"arguments",chosen}}})}});
                }
                if (input.at("task").at("revision") != 1 || answer.at("status") != "answered" || answer.at("answerMessage") != 4 ||
                    answer.at("answer") != "左边" || answer.at("placementCells") != Json::array({wall.at(0)}))
                    throw std::runtime_error("placement answer lost its selected cell or original task revision");
                return probe_placement("unchosen-append",unchosen);
            }
            if (event.at("sequence") == (direct_place ? 5 : 3)) {
                if (input.at("activeMotion").at("status") != "running") throw std::runtime_error("no accepted work at revision change");
                if (direct || navigation) old_work = input.at("activeMotion").at("work");
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
                old_work = input.at("activeMotion").at("work");
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
        for (std::string line; std::getline(trace,line);) {
            if (response_timing) {
                if (line.find("\"category\":\"mc.partner.action\"") != line.npos &&
                    line.find("\"sent\":true") != line.npos && line.find("\"text\":\"两段都走完了。\"") != line.npos) return true;
                continue;
            }
            if (line.find("\"kind\":\"run.exited\"") != line.npos && line.find("\"result\":\"succeeded\"") != line.npos &&
                ((repair_observe || repair_plan) ? line.find("\"run\":2,") != line.npos :
                    line.find(repair_plan_observe ? "看过了，我还在。" : question_expiry ? "这段内部结算" :
                        layered_blocked || layered_stock ? "第一块垫好了" : "plan exit checked") != line.npos)) return true;
        }
        return false;
    };
    bool stopping = false;
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(question_expiry ? 52 : late_exit ? 15 : 8);
    while (std::chrono::steady_clock::now() < deadline) {
        if (host.advance_plugins() == runtime::RunResult::failed) throw std::runtime_error("plan revision loop failed");
        if (stopping && host.companion_status()->closed) return 0;
        if (!stopping && checked && host.companion_status()->settled_event ==
            (failed_question ? 4u : question_pair ? 12u : air_budget ? 3u : repair_observe || repair_plan || pointed_support || observation_batch || observe_dig_scope || continuation_budget ? 2u : restart_route || rejoin || (direct_place && !stale_place) ? 5u :
                pointed_craft || attention || repair_plan_observe || stale_place ? 4u : 3u) &&
            receipt_turn_finished() &&
            ((!restart_route && !layered) || std::filesystem::exists(root/"original-follow-resumed"))) {
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

// 用固定响应核对单条与同批原话的显式发言、最终文本及投递终态。
// 步骤：接收真实插件输入 → 按原话返回固定发言 → 等原回复结算后关闭。
int verify_reply_delivery(const std::filesystem::path& package, const std::filesystem::path& root, std::wstring_view variant) {
    using namespace dfy;
    using namespace model;
    using Json = nlohmann::json;
    const bool finish = variant.ends_with(L"-finish");
    const bool late = variant.ends_with(L"-late");
    const bool retired = variant.ends_with(L"-retired");
    const bool capacity = variant.ends_with(L"-capacity");
    const bool lookup = variant.ends_with(L"-lookup");
    const bool batch = variant.ends_with(L"-batch");
    const auto outcome = finish || variant.ends_with(L"-sent") ? "sent" : variant.ends_with(L"-unknown") ? "unknown" : "not_sent";
    const bool internal = variant.ends_with(L"-internal");
    const std::string reply = "我在这儿。";
    std::atomic_bool received = false;
    std::atomic_uint64_t fill_session = 0;
    std::atomic_uint64_t original_session = 0, new_session = 0;
    std::binary_semaphore filled(0);
    app::ApplicationRunConfig run;
    run.trace_directory = root;
    app::Application host({},root/"packages",root/"user",Backend{[&](const auto&, const Request& request) -> Response {
        const auto& message = request.messages.back();
        if (message.role == Role::tool) {
            if (lookup) {
                std::size_t checked = 0;
                for (const auto& value : request.messages) if (value.role == Role::tool) {
                    const auto body = Json::parse(value.content.substr(value.content.find('{')));
                    if (value.tool_call_id == "original") {
                        if (body.at("workReceipt").at("work") != "long-work" ||
                            !body.at("workReceipt").at("body").at("sample").get<std::string>().ends_with("尾部原文"))
                            throw std::runtime_error("wrong expanded work receipt");
                    } else if (body.at("status") != "unavailable" || body.contains("workReceipt"))
                        throw std::runtime_error("foreign or missing work receipt exposed");
                    ++checked;
                }
                if (checked != 3) throw std::runtime_error("lookup did not settle all reads");
                received = true;
                return {{Role::assistant,R"({"internal":"checked"})"}};
            }
            if (batch) {
                const auto result = Json::parse(message.content.substr(message.content.find('{')));
                if (message.tool_call_id != "repeat-first" || result.at("status") != "already_requested" ||
                    result.at("bridgeWork").get<std::string>().empty()) throw std::runtime_error("lost original reply work");
                return {{Role::assistant,reply}};
            }
            if (finish) throw std::runtime_error("confirmed closing reply submitted another model step");
            const auto result = Json::parse(message.content.substr(message.content.find('{')));
            if (capacity) {
                if (message.tool_call_id!="explicit-reply" || result.at("reasonCode")!="work_receipt_capacity" ||
                    message.content.find("effect=not_applied")==std::string::npos || received.exchange(true))
                    throw std::runtime_error("full ledger did not stop the original game write");
                return {{Role::assistant,R"({"internal":"keep quiet"})"}};
            }
            if (result.at("delivery") != outcome || received.exchange(true)) throw std::runtime_error("lost actual reply delivery state");
            return {{Role::assistant,internal ? R"({"internal":"keep quiet"})" : reply}};
        }
        const auto input = Json::parse(message.content);
        if (input.at("events").back().at("sequence") == 1) {
            if (lookup) {
                fill_session = input.at("factFrame").at("epoch").get<std::uint64_t>();
                if (!filled.try_acquire_for(std::chrono::seconds(4))) throw std::runtime_error("missing receipt fixture");
            }
            return {{Role::assistant,""}};
        }
        if (lookup) {
            const auto& fact = input.at("hostWorkReceipts").at(0);
            const auto epoch = input.at("factFrame").at("epoch").get<std::uint64_t>();
            const auto receipt = fact.at("effectReceipt").get<std::uint64_t>();
            const auto observe = std::ranges::find_if(request.tools,[](const auto& entry) { return entry.name.ends_with("_observe"); });
            if (!fact.at("effect").contains("sha256") || message.content.find("尾部原文") != std::string::npos || observe == request.tools.end() ||
                request.messages.front().role != Role::system || request.messages.front().content.find("work_receipt") == std::string::npos ||
                observe->parameters.find("work_receipt") == std::string::npos)
                throw std::runtime_error("model did not receive a compact reference");
            const auto read = [&](std::string id, std::uint64_t epoch_value, std::uint64_t number) {
                return ToolCall{std::move(id),observe->name,Json{{"mode","work_receipt"},{"epoch",epoch_value},{"receipt",number}}.dump()};
            };
            return {{Role::assistant,"",{read("original",epoch,receipt),read("foreign",epoch+1,receipt),read("missing",epoch,receipt+100)}},FinishReason::tool_calls};
        }
        if (retired) {
            const auto epoch = input.at("factFrame").at("epoch").get<std::uint64_t>();
            if (input.at("events").back().at("sequence") == 2) { original_session = epoch; return {{Role::assistant,reply}}; }
            if (epoch == original_session || std::ranges::any_of(request.messages,[&](const Message& value) {
                return value.content.find(reply) != std::string::npos;
            })) throw std::runtime_error("old delivery leaked into new binding");
            new_session = epoch;
            return {{Role::assistant,R"({"internal":"keep quiet"})"}};
        }
        if (late) return {{Role::assistant,reply}};
        if (capacity) {
            fill_session = input.at("factFrame").at("epoch").get<std::uint64_t>();
            if (!filled.try_acquire_for(std::chrono::seconds(4))) throw std::runtime_error("work index fixture did not fill");
        }
        const auto tool = std::ranges::find_if(request.tools,[](const auto& entry) { return entry.name.ends_with("_reply"); });
        if (batch) {
            const auto& events = input.at("events");
            if (events.size() != 2 || events.front().at("text") != events.back().at("text") ||
                input.at("replyOrigin").at("sourceMessage") != events.back().at("sequence") || tool == request.tools.end())
                throw std::runtime_error("expected both independent originals in one decision");
            const Json first{{"text",reply},{"sourceMessage",events.front().at("sequence")}};
            Response response{{Role::assistant,"",{{"first",tool->name,first.dump()}}},FinishReason::tool_calls};
            if (events.back().at("sequence") == 5)
                response.message.tool_calls.push_back({"last",tool->name,Json{{"text",reply}}.dump()});
            auto repeated = first;
            repeated["text"] = "这仍是给第一句的回复。";
            response.message.tool_calls.push_back({"repeat-first",tool->name,repeated.dump()});
            return response;
        }
        if (received || tool == request.tools.end()) throw std::runtime_error("reply was unavailable or the turn repeated");
        Json args{{"text",std::string_view(outcome) == "not_sent" ? "第一行。\n第二行。" : reply}};
        if (finish || std::string_view(outcome) != "sent") args["finish"] = true;
        Response response{{Role::assistant,"",{{"explicit-reply",tool->name,args.dump()}}},FinishReason::tool_calls};
        if (finish) {
            const auto observe = std::ranges::find_if(request.tools,[](const auto& entry) { return entry.name.ends_with("_observe"); });
            if (observe == request.tools.end()) throw std::runtime_error("missing sibling observation");
            response.message.tool_calls.push_back({"sibling-observe",observe->name,"{}"});
            received = true;
        }
        return response;
    }},std::move(run));
    host.plugins().install(package);
    if (host.plugins().load("mc.companion")) throw std::runtime_error("could not load reply package");
    const auto id = host.open_companion("mc.companion");
    host.listen_companion(id);
    bool stopping = false;
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(batch ? 25 : 6);
    std::uint64_t announced = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        if (host.advance_plugins() == runtime::RunResult::failed) throw std::runtime_error("reply loop failed");
        if (const auto session = fill_session.exchange(0)) {
            const auto owner = host.plugins().instance("mc.companion")->generation();
            if (lookup) {
                host.subagents().settle(owner,session,{session,1,"long-work",R"({"skill":"observe"})",DFY_V2_WORK_ACCEPTED,1});
                host.subagents().settle(owner,session,{session,2,"long-work",Json{{"sample",std::string(4096,'x')+"尾部原文"}}.dump(),DFY_V2_WORK_EFFECT,0,true});
                host.subagents().settle(owner,session,{session,3,"long-work","{}",DFY_V2_WORK_EXIT});
            } else for (unsigned n=0;n<128;++n)
                host.subagents().settle(owner,session,{session,1,"pending-"+std::to_string(n),"{}",DFY_V2_WORK_ACCEPTED});
            filled.release();
        }
        if (stopping && host.companion_status()->closed) return 0;
        const auto settled = host.companion_status()->settled_event;
        if (retired && settled >= 2 && !stopping) {
            if (!announced) { std::ofstream(root/"old-settled").put('1'); announced = 2; }
            if (new_session && settled == 5) {
                if (announced != 5) { std::ofstream(root/"new-bound").put('1'); announced = 5; }
                const auto owner = host.plugins().instance("mc.companion")->generation();
                const auto receipt = host.subagents().read_receipt(owner,original_session,original_session,4);
                if (receipt) {
                    const auto late_fact = Json::parse(*receipt);
                    if (!late_fact.at("finalEffect").get<bool>() || late_fact.at("body").at("result").at("delivery") != "sent" ||
                        host.subagents().read_receipt(owner,new_session,new_session,4))
                        throw std::runtime_error("late exit was not settled on its original binding");
                    received = true;
                }
            }
        }
        if (late && settled == 2) {
            const auto messages = host.companion_messages(id,0).messages;
            const auto player = std::ranges::find(messages,app::CompanionMessageKind::utterance,&app::CompanionMessage::kind);
            const auto reports = std::ranges::count(messages,app::CompanionMessageKind::report,&app::CompanionMessage::kind);
            if (reports > 1) throw std::runtime_error("late delivery duplicated the original reply");
            if (reports == 1 && player != messages.end()) {
                const auto result = std::ranges::find(messages,app::CompanionMessageKind::report,&app::CompanionMessage::kind);
                if (result->text != reply || result->reply_to != player->revision) throw std::runtime_error("late reply lost its source");
                received = true;
            }
        }
        if (batch && settled > announced && (settled == 1 || settled == 3)) {
            announced = settled;
            std::ofstream(root/("settled-"+std::to_string(settled))).put('1');
        }
        if (!stopping && (batch ? settled == 5 : received && settled == (retired ? 5u : 2u))) {
            host.stop_companion(id);
            stopping = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error("reply did not reach actual settlement");
}
}

namespace {
// 用等效采掘请求核对直调、顺序队列和计划的授权与保护结果。
// 步骤：绑定任务范围并提出确认 → 用普通采掘和补库存尝试原候选 → 核对合法取材的原工作回执。
int verify_refused_collection(const std::filesystem::path& package, const std::filesystem::path& root, std::wstring_view variant) {
    using namespace dfy;
    using namespace model;
    using Json = nlohmann::json;
    const bool sequence = variant.ends_with(L"-queue"), plan = variant.ends_with(L"-plan");
    std::atomic_uint receipts = 0;
    std::uint64_t pending_item = 0;
    app::ApplicationRunConfig run;
    run.trace_directory = root;
    app::Application host({},root/"packages",root/"user",Backend{[&](const auto&, const Request& request) -> Response {
        const Response silent{{Role::assistant,R"({"internal":"fixed fixture settled"})"}};
        const auto call = [&](std::string_view name, std::string call_id, Json args) -> Response {
            const auto tool = std::ranges::find_if(request.tools,[&](const auto& entry) { return entry.name.ends_with(name); });
            if (tool == request.tools.end()) throw std::runtime_error("required collection test tool was not visible");
            return {{Role::assistant,"",{{std::move(call_id),tool->name,args.dump()}}},FinishReason::tool_calls};
        };
        // 以当前测试入口提交同一种实际取材意图。
        // 步骤：构造固定单格或目标库存 → 以对应入口包装候选。
        const auto collect = [&](bool stock, bool allowed, std::string call_id = "collect") -> Response {
            Json args = stock ? Json{{"item","dirt"},{"requiredStock",1},{"block",allowed ? "coarse_dirt" : "dirt"}} :
                Json{{"x",2},{"y",-61},{"z",0},{"block","dirt"}};
            if (!sequence && !plan) return call(stock ? "_gather_ground" : "_dig_block",std::move(call_id),std::move(args));
            if (sequence && !stock) args["count"] = 1;
            Json step{{"skill",stock ? "gather_ground" : plan ? "dig_block" : "collect"},{"arguments",std::move(args)}};
            if (plan) step["id"] = "collection";
            return call("_queue_actions",std::move(call_id),{{"operation",plan ? "plan" : "append"},{"steps",Json::array({step})}});
        };
        const auto& message = request.messages.back();
        // 用实际回执核对未授权候选无修改，以及合法取材只影响未受保护的格。
        // 步骤：检查拒绝或成功的实际证据 → 标记原回执已消费以推进夹具。
        const auto verify = [&](const Json& result) {
            if (receipts < 2) {
                if (result.value("reasonCode","") != "approval_required" || result.at("broken") != 0 || !result.at("changes").empty())
                    throw std::runtime_error("refused collection changed the world: "+result.dump());
            } else if (receipts != 2 || result.at("status") != "done" || result.at("stock").at("after") != 1 ||
                result.at("inventoryDelta").at("dirt") != 1 || result.at("changes") != Json::array({
                    {{"x",5},{"y",-61},{"z",0},{"before","coarse_dirt"},{"after","air"}}})) {
                throw std::runtime_error("approved scope did not spare the protected cells: "+result.dump());
            }
            std::ofstream(root/("collection-receipt-"+std::to_string(++receipts))).put('1');
        };
        if (message.role == Role::tool) {
            if (message.tool_call_id == "bind")
                return call("_skills","discover",{{"categories",{"dig_break"}}});
            if (message.tool_call_id == "discover")
                return call("_gather_ground","ask",{{"item","dirt"},{"block","dirt"},{"requiredStock",1},{"approval","game"}});
            if (message.tool_call_id == "pending") {
                const auto result = Json::parse(message.content.substr(message.content.find('{')));
                if (result.at("status") != "blocked" || result.at("reason") != "approval_required" ||
                    !pending_item || result.at("item") != pending_item || result.at("broken") != 0)
                    throw std::runtime_error("collection entered while confirmation was pending: "+result.dump());
                std::ofstream(root/"collection-pending-checked").put('1');
            } else if (message.tool_call_id == "collect") {
                const auto result = Json::parse(message.content.substr(message.content.find('{')));
                if (result.at("status") != "accepted" && result.at("status") != "queued") verify(result);
            }
            return silent;
        }
        const auto input = Json::parse(message.content);
        if (input.contains("planSettlements")) { verify(input.at("planSettlements").front().at("nodes").front().at("result")); return silent; }
        if (input.contains("workSettlement")) { verify(primary_receipt(input).at("result")); return silent; }
        if (input.at("trigger") == "skills.expanded" && input.contains("pendingApproval")) {
            pending_item = input.at("pendingApproval").at("item").get<std::uint64_t>();
            return collect(false,false,"pending");
        }
        if (input.at("events").empty()) return silent;
        const auto& event = input.at("events").back();
        if (event.at("sequence") == 2)
            return call("_task_update","bind",{{"goal","从地面取材并保留墙边"},{"next","player"},{"collection","ground"},
                {"target",{{"message",2},{"cells",Json::array({{{"x",3},{"y",-60},{"z",0}}})}}}});
        if (event.at("sequence") == 4) return collect(false,false);
        if (event.at("sequence") == 5) return collect(true,false);
        if (event.at("sequence") == 6) return collect(true,true);
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
        if (!stopping && receipts == 3 && host.companion_status()->settled_event == 6) {
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
        if (argc == 4 && std::wstring_view(argv[3]).starts_with(L"partner-refused-collection"))
            return verify_refused_collection(argv[1],argv[2],argv[3]);
        if (argc == 3) {
            verify_follow_windows();
            verify_collection_effect();
            verify_target_evidence();
            verify_opening_geometry();
            verify_opening_selection();
            verify_below_feet_evidence();
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
                !request.messages.front().content.starts_with("【职责与信息来源 v1】") ||
                request.messages.front().content.find("【人格与风格 v1】\n你叫大肥鱼。") == std::string::npos ||
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
                if (!follow && (active.at("progress").at("targetCell").at("x") != 4 ||
                    active.at("progress").at("phase") != "moving" || active.at("progress").at("distance") != 3.25))
                    throw std::runtime_error("active walk lost its latest measured position or distance");
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
