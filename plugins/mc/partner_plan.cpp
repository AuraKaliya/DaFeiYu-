#include "partner_loop.hpp"
#include "partner_effect.hpp"
#include <dafeiyu/tool_abi_v2.h>
#include <algorithm>
#include <stdexcept>

namespace dfy::mc {
namespace {
using Json = nlohmann::json;

// 核对各节点实际使用的参数，准备传给现有游戏技能的参数。
// 步骤：按技能选择约束 → 排除不属于该技能的键 → 固定动作参数。
bool prepare_action(std::string_view skill, Json& args) {
    const auto name = [&](const char* key) {
        return args.contains(key) && args.at(key).is_string() && !args.at(key).get_ref<const std::string&>().empty() &&
            args.at(key).get_ref<const std::string&>().size() <= 90;
    };
    const auto quantity = [&](const char* key) {
        return args.contains(key) && args.at(key).is_number_integer() && args.at(key) >= 1 && args.at(key) <= 32;
    };
    if (skill == "fill_target") return args.size() == 1 && name("block");
    if (skill == "place") return args.size() == 4 && name("block") && valid_action_cell(args);
    if (skill == "move") {
        if (args.size() != 3 || !valid_action_cell(args)) return false;
        args["exact"] = true;
        return true;
    }
    if (skill == "craft") return args.size() == 2+args.count("ingredient") && name("item") && quantity("count") &&
        !item_name(args.at("item").get_ref<const std::string&>()).empty() && (!args.contains("ingredient") ||
            (name("ingredient") && !item_name(args.at("ingredient").get_ref<const std::string&>()).empty()));
    if (skill == "route") return args.size() == 2 && name("route") && quantity("laps") && args.at("laps") <= 8;
    if (skill == "observe") return args.empty() || (args.contains("message") &&
        args.at("message").is_number_unsigned() && args.at("message") > 0 &&
        (args.size() == 1 || (args.size() == 2 && (args.value("focus","") == "hit" || args.value("focus","") == "openings"))));
    if (skill == "gather_ground") return valid_stock_request(args);
    return skill == "dig_block" && args.size() == 4+args.count("message") && name("block") && valid_action_cell(args) &&
        (!args.contains("message") || (args.at("message").is_number_unsigned() && args.at("message") > 0));
}

// 保留推进图所需的结果，不在信箱中复制一次完整的背景观察。
Json plan_evidence(const Json& result) {
    Json evidence = Json::object();
    for (const auto name : {"status","reason","reasonCode","broken","stock","inventoryDelta","changes","cells",
        "sampledAtMs","route","item","count","position","arrived","blocks","placed","materialBefore","materialAfter","craft","craftAttempted"})
        if (result.contains(name)) evidence[name] = result.at(name);
    return evidence;
}

// 使用澄清后确认的实际木种，为玩家只说木板时固定具体产物。
std::string craft_item(std::string_view item, std::string_view block) {
    item = item_name(item);
    if (item != "planks") return std::string(item);
    block = item_name(block);
    if (block.starts_with("stripped_")) block.remove_prefix(9);
    if (block.ends_with("_log")) block.remove_suffix(4);
    else if (block.ends_with("_stem")) block.remove_suffix(5);
    else return {};
    return std::string(block)+"_planks";
}
}

// 记下计划提交的确定性拒绝，并保留一次有界修订后的自然结算。
// 步骤：归还成功提交 → 单独结算待答步骤 → 为其他拒绝记录修订额度和原任务身份。
Json PartnerLoop::report_plan_attempt(std::uint64_t invocation, Json result) {
    if (result.at("status") == "queued") {
        plan_rejection_ = nullptr;
        return result;
    }
    const auto reason = result.value("reason","");
    if (reason == "answer_target_mismatch" || (reason == "waiting_for_player" && placement_choice_pending() &&
        result.at("item") == placement_choice_.item)) {
        result["attempt"] = invocation;
        result["retryAllowed"] = false;
        publish("mc.partner.plan",{{"status","step_blocked"},{"revision",task_.revision()},{"order",order_},{"result",result}});
        return result;
    }
    if (result.at("status") == "invalid_plan" &&
        (++plan_failures_ >= 3 || ++repair_failures_["plan:"+reason] >= 2)) {
        plan_repair_exhausted_ = true;
        task_.disarm();
    }
    result["attempt"] = invocation;
    result["retryAllowed"] = !plan_repair_exhausted_ && result.at("status") == "invalid_plan";
    if (reason == "unsupported_plan_skill") result["planNodes"] = plan_skills();
    plan_rejection_ = {{"status","rejected"},{"revision",task_.revision()},{"order",order_},{"result",result}};
    if (task_.id()) plan_rejection_["task"] = task_.id();
    publish("mc.partner.plan",plan_rejection_);
    return result;
}

// 在当前世界验证一到四个节点及全部依赖，再原子替换上次任务图。
// 步骤：校验槽位和节点参数 → 固定前置编号 → 拓扑分层并登记。
Json PartnerLoop::submit_plan(std::uint64_t invocation, const Json& arguments) {
    if (!invocation || (plan_ && plan_->active) || !queued_.empty() || (motion_ && motion_->skill != "follow"))
        return {{"status","busy"}};
    auto steps = arguments.value("steps",Json::array());
    if (arguments.contains("work") || !steps.is_array() || steps.empty() || steps.size() > 4)
        return {{"status","invalid_plan"},{"reason","expected_one_to_four_steps"}};
    const auto resolves = arguments.value("resolves",std::uint64_t{});
    if (resolves) {
        if (steps.size() != 1 || steps.front().at("skill") != "dig_block")
            return {{"status","invalid_plan"},{"reason","expected_one_pointed_dig"}};
        if (has_pickup_continuation(resolves)) {
            const auto& next = question_->after_pickup;
            const auto block = steps.front().at("arguments").value("block",std::string{});
            const auto item = craft_item(next.at("item").get_ref<const std::string&>(),block);
            const auto pickup = next.value("pickup",block);
            if (item.empty() || item_name(pickup).empty())
                return {{"status","invalid_plan"},{"reason","pickup_recipe_unverified"}};
            steps.front()["pickup"] = pickup;
            const auto parent = steps.front().at("id");
            steps.push_back({{"id","after_pickup"},{"skill","craft"},{"after",Json::array({parent})},
                {"arguments",{{"item",item},{"count",next.at("count")},{"ingredient",pickup}}}});
        }
    }
    Plan plan{invocation,task_.revision(),order_,request_,plan_ && plan_->revision == task_.revision() ? plan_->id : 0};
    for (const auto& step : steps) {
        auto id = step.value("id",std::string{});
        const auto skill = step.at("skill").get<std::string>();
        auto args = step.at("arguments");
        const auto* action = partner_action(skill);
        const auto invalid = [&](std::string_view reason) {
            return Json{{"status","invalid_plan"},{"reason",reason},{"node",id},{"skill",skill}};
        };
        if (id.empty() || id.size() > 32 || id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") != id.npos ||
            std::ranges::find(plan.nodes,id,&PlanNode::id) != plan.nodes.end()) return invalid("invalid_node_id");
        if (!action || !action->plan) return invalid("unsupported_plan_skill");
        const auto pickup = step.value("pickup",std::string{});
        const auto required = item_name(pickup);
        if ((step.contains("pickup") && (skill != "dig_block" || required.empty())) || !prepare_action(skill,args))
            return invalid("invalid_node_arguments");
        if (skill == "route" && (!task_.bind_route(args) ||
            std::ranges::find(plan.nodes,skill,&PlanNode::skill) != plan.nodes.end())) return invalid("route_not_verified");
        if (skill == "dig_block") {
            const auto pointed = pointed_dig(session_,args.value("message",std::uint64_t{}),resolves,args);
            if (!pointed.has_value() || (resolves && !*pointed)) return invalid("pointed_block_unverified");
            args.erase("message");
        }
        if (skill == "gather_ground" && requires_pointed_dig(session_)) return invalid("pointed_block_unverified");
        if (action->executor == "collect") bind_collection(skill,args);
        if (const auto reason = bind_placement_target(action->executor,args); !reason.empty()) return invalid(reason);
        if (requires_approval(action->executor,args)) return {{"status","blocked"},{"reason","approval_required"},{"node",id}};
        auto work = "plan."+std::to_string(invocation)+"."+std::to_string(plan.nodes.size()+1);
        plan.nodes.push_back({std::move(id),std::move(work),action->name,
            std::move(args),{},std::string(required)});
    }
    for (std::size_t index = 0; index < plan.nodes.size(); ++index) {
        const auto after = steps.at(index).value("after",std::vector<std::string>{});
        auto& dependencies = plan.nodes[index].after;
        for (const auto& id : after) {
            const auto node = std::ranges::find(plan.nodes,id,&PlanNode::id);
            const auto position = static_cast<std::size_t>(node-plan.nodes.begin());
            if (node == plan.nodes.end() || std::ranges::find(dependencies,position) != dependencies.end())
                return {{"status","invalid_plan"},{"reason","invalid_dependency"},{"node",plan.nodes[index].id},{"dependency",id}};
            if (plan.nodes[index].skill == "craft" && node->skill == "dig_block" && node->pickup.empty())
                return {{"status","invalid_plan"},{"reason","pickup_required"},{"node",node->id}};
            dependencies.push_back(position);
        }
    }
    for (std::size_t layer = 1; layer <= plan.nodes.size(); ++layer)
        for (auto& node : plan.nodes)
            if (!node.layer && std::ranges::all_of(node.after,[&](auto index) {
                const auto previous = plan.nodes[index].layer;
                return previous > 0 && previous < layer;
            })) node.layer = layer;
    if (std::ranges::any_of(plan.nodes,[](const auto& node) { return !node.layer; }))
        return {{"status","invalid_plan"},{"reason","cyclic_dependencies"}};

    Json waiting;
    bool ready = false;
    for (const auto& node : plan.nodes) if (node.after.empty()) {
        const auto executor = partner_action(node.skill)->executor;
        auto question = action_question(session_,executor,node.arguments,resolves);
        if (question.is_null()) ready = true;
        else if (waiting.is_null()) {
            waiting = std::move(question);
            waiting["node"] = node.id;
        }
    }
    if (!ready) return waiting;

    plan_ = std::move(plan);
    if (resolves) complete_pointed_question();
    wait_for_work_ = true;
    publish("mc.partner.plan",{{"status","queued"},{"plan",plan_context()}});
    return {{"status","queued"},{"plan",plan_->id}};
}

// 返回可随新消息交付的图修订、替换关系和每个节点的原 ID。
Json PartnerLoop::plan_context() const {
    const auto& plan = *plan_;
    constexpr std::array statuses{"pending","running","done","failed","skipped"};
    Json nodes = Json::array();
    for (const auto& node : plan.nodes) {
        nodes.push_back({{"id",node.id},{"skill",node.skill},{"layer",node.layer},{"work",node.work},
            {"status",statuses.at(static_cast<std::size_t>(node.state))}});
        if (!node.pickup.empty()) nodes.back()["requiresPickup"] = node.pickup;
        if (!node.after.empty()) {
            nodes.back()["after"] = Json::array();
            for (const auto parent : node.after) nodes.back()["after"].push_back(plan.nodes[parent].id);
        }
    }
    const auto complete = std::ranges::all_of(plan.nodes,[](const auto& node) { return node.state == PlanNode::State::done; });
    Json context{{"id",plan.id},{"revision",plan.revision},{"supersedes",plan.supersedes},{"order",plan.order},
        {"status",plan.active ? "running" : !plan.cancellation.empty() ? "cancelled" : complete ? "done" : "blocked"},
        {"nodes",std::move(nodes)}};
    if (!plan.cancellation.empty()) context["cancelReason"] = plan.cancellation;
    return context;
}

// 记录一次确切节点退出，以实际动作验证决定能否解除后续依赖。
void PartnerLoop::finish_plan_node(std::size_t index, bool ok, const Json& result) {
    auto& node = plan_->nodes.at(index);
    if (node.state != PlanNode::State::running) return;
    node.state = ok ? PlanNode::State::done : result.at("status") == "skipped" ? PlanNode::State::skipped : PlanNode::State::failed;
    node.result = plan_evidence(result);
    if (!ok && !node.pickup.empty() && result.at("status") == "done") {
        node.result["status"] = "blocked";
        node.result["reasonCode"] = "pickup_unverified";
        node.result["gameStatus"] = result.at("status");
    }
}

// 停止未接纳节点并让已接纳的前后台动作进行真实取消。
// 步骤：标记计划取消 → 清除等待队列 → 保留在途动作直至读回。
void PartnerLoop::cancel_plan(std::string_view reason) {
    if (!plan_ || !plan_->active || !plan_->cancellation.empty()) return;
    plan_->cancellation = reason;
    clear_queue(reason,plan_->id);
    if (motion_ && motion_->plan_node) motion_->stopping = true;
    for (auto& node : plan_->nodes) if (node.state == PlanNode::State::pending) {
        node.state = PlanNode::State::skipped;
        node.result = {{"status","skipped"},{"reason",reason}};
    }
}

// 同层后台节点独立读取，前台节点共用游戏队列和跟随交接。
// 步骤：处理取消和原观察退出 → 接纳当前层就绪节点 → 汇总一次终态并推进下一层。
void PartnerLoop::advance_plan(const BridgeConfig& config, bool closing) {
    if (!plan_ || !plan_->active) return;
    auto& plan = *plan_;
    using State = PlanNode::State;
    if (closing || feed_.gap()) cancel_plan("stale");
    else if (plan.revision != task_.revision()) cancel_plan("task_changed");

    const auto can_start = feed_.ready() && feed_.pending().empty() && (!request_ || order_ <= plan.order);
    Json results = Json::array();
    for (std::size_t index = 0; index < plan.nodes.size(); ++index) {
        auto& node = plan.nodes[index];
        if (node.layer != plan.layer) continue;
        if (node.state == State::pending && can_start) {
            const bool ready = std::ranges::all_of(node.after,[&](auto parent) { return plan.nodes[parent].state == State::done; });
            const auto skill = partner_action(node.skill)->executor;
            node.state = State::running;
            if (!ready) finish_plan_node(index,false,{{"status","skipped"},{"reason","prerequisite_failed"}});
            else if (const auto question = action_question(session_,skill,node.arguments); !question.is_null())
                finish_plan_node(index,false,question);
            else if (skill.empty()) finish_plan_node(index,false,{{"status","blocked"},{"reasonCode","skill_unavailable"},
                {"item",node.arguments.at("item")},{"count",node.arguments.at("count")}});
            else if (skill == "observe") {
                const auto request = Json{{"context",{{"generation",session_world_},{"order",feed_.cursor()}}},
                    {"arguments",node.arguments}}.dump();
                node.reading = std::make_unique<GameWork>(GameAction::partner,config,node.work,request,std::string(skill));
            } else {
                auto args = node.arguments;
                if (node.skill == "route") task_.accept_route(args);
                queued_.push_back({plan.order,plan.request,plan.id,plan.revision,node.work,skill,std::move(args),node.pickup,true,false,index});
                pause_follow();
            }
        }
        if (node.reading) {
            std::optional<GameResult> read;
            try { read = node.reading->poll(!plan.cancellation.empty(),false,!feed_.recovering()); }
            catch (...) { read = GameResult{false,GameEffect::unknown,{}}; }
            if (read) {
                const auto result = read->payload.empty() ? Json{{"status","unconfirmed"}} : Json::parse(read->payload);
                const Json fact{{"generation",session_world_},{"order",plan.order},{"session",session_},{"request",plan.request},
                    {"invocation",plan.id},{"bridgeWork",node.work},{"skill","observe"},{"effect",DFY_V2_TOOL_NOT_APPLIED},{"result",result}};
                if (!closing) task_.record(fact,!task_.id() || plan.revision == task_.revision());
                publish("mc.partner.action",fact);
                finish_plan_node(index,read->success && result.at("status") == "done",result);
                node.reading.reset();
            }
        }
        if (node.state != State::pending && node.state != State::running)
            results.push_back({{"id",node.id},{"skill",node.skill},{"work",node.work},{"result",node.result}});
    }
    const auto members = std::ranges::count(plan.nodes,plan.layer,&PlanNode::layer);
    if (results.size() != static_cast<std::size_t>(members)) return;
    Json layer{{"status","settled"},{"plan",plan.id},{"layer",plan.layer},{"nodes",std::move(results)}};
    if (!plan.cancellation.empty()) layer["cancelReason"] = plan.cancellation;
    publish("mc.partner.plan",layer);
    if (!task_.id() || plan.revision == task_.revision()) plan_receipts_.push_back(std::move(layer));
    ++plan.layer;
    plan.active = std::ranges::any_of(plan.nodes,[&](const auto& node) { return node.layer == plan.layer; });
    if (!plan.active && plan.revision == task_.revision()) {
        const auto route = std::ranges::find(plan.nodes,"route",&PlanNode::skill);
        if (route != plan.nodes.end() && route->state != State::skipped)
            task_.finish_route(std::ranges::all_of(plan.nodes,[](const auto& node) { return node.state == State::done; }));
    }
}
}
