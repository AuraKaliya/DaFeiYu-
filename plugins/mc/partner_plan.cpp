#include "partner_loop.hpp"
#include <dafeiyu/tool_abi_v2.h>
#include <algorithm>
#include <stdexcept>

namespace dfy::mc {
namespace {
using Json = nlohmann::json;

// 核对各节点实际使用的参数，准备传给现有游戏技能的参数。
// 步骤：按技能选择约束 → 排除不属于该技能的键 → 固定采掘语义。
bool prepare_action(std::string_view skill, Json& args) {
    const auto name = [&](const char* key) {
        return args.contains(key) && args.at(key).is_string() && !args.at(key).get_ref<const std::string&>().empty() &&
            args.at(key).get_ref<const std::string&>().size() <= 90;
    };
    const auto quantity = [&](const char* key) {
        return args.contains(key) && args.at(key).is_number_integer() && args.at(key) >= 1 && args.at(key) <= 32;
    };
    if (skill == "fill_target") return args.size() == 1 && name("block");
    if (skill == "craft") return args.size() == 2 && name("item") && quantity("count");
    if (skill == "observe") return args.empty() || (args.contains("message") &&
        args.at("message").is_number_unsigned() && args.at("message") > 0 &&
        (args.size() == 1 || (args.size() == 2 && (args.value("focus","") == "hit" || args.value("focus","") == "openings"))));
    if (skill == "gather_ground") {
        if (!name("item") || !quantity("requiredStock") ||
            (args.size() != 2 && (args.size() != 3 || !name("block")))) return false;
        args["count"] = args.at("requiredStock");
        args["source"] = "ground";
        return true;
    }
    if (skill != "dig_block" || args.size() != 4 || !name("block")) return false;
    for (const auto key : {"x","y","z"}) if (!args.contains(key) || !args.at(key).is_number_integer() ||
        args.at(key) < -30000000 || args.at(key) > 30000000) return false;
    args["source"] = "visible";
    args["count"] = 1;
    return true;
}

// 保留推进图所需的结果，不在信箱中复制一次完整的背景观察。
Json plan_evidence(const Json& result) {
    Json evidence = Json::object();
    for (const auto name : {"status","reason","reasonCode","stock","inventoryDelta","changes","cells","sampledAtMs"})
        if (result.contains(name)) evidence[name] = result.at(name);
    return evidence;
}
}

// 在当前世界验证一到四个节点及全部依赖，再原子替换上次任务图。
// 步骤：校验槽位和节点参数 → 固定前置编号 → 拓扑分层并登记。
Json PartnerLoop::submit_plan(std::uint64_t invocation, const Json& arguments) {
    if (!invocation || (plan_ && plan_->active) || !queued_.empty() || (motion_ && motion_->skill != "follow"))
        return {{"status","busy"}};
    const auto& steps = arguments.at("steps");
    if (arguments.contains("work") || !steps.is_array() || steps.empty() || steps.size() > 4)
        return {{"status","invalid_plan"}};
    Plan plan{invocation,task_.revision(),order_,request_,plan_ && plan_->revision == task_.revision() ? plan_->id : 0};
    for (const auto& step : steps) {
        auto id = step.at("id").get<std::string>();
        const auto skill = step.at("skill").get<std::string>();
        auto args = step.at("arguments");
        const auto tool = std::ranges::find(partner_skills,skill,&PartnerSkill::name);
        if (id.empty() || id.size() > 32 || id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") != id.npos ||
            std::ranges::find(plan.nodes,id,&PlanNode::id) != plan.nodes.end() ||
            (skill != "craft" && tool == partner_skills.end()) || step.contains("pickup") ||
            !prepare_action(skill,args) || (skill == "fill_target" && !task_.target_ready()))
            return {{"status","invalid_plan"}};
        auto work = "plan."+std::to_string(invocation)+"."+std::to_string(plan.nodes.size()+1);
        plan.nodes.push_back({std::move(id),std::move(work),skill == "craft" ? "craft" : tool->name,std::move(args)});
    }
    for (std::size_t index = 0; index < plan.nodes.size(); ++index) {
        const auto after = steps.at(index).value("after",std::vector<std::string>{});
        auto& dependencies = plan.nodes[index].after;
        for (const auto& id : after) {
            const auto node = std::ranges::find(plan.nodes,id,&PlanNode::id);
            const auto position = static_cast<std::size_t>(node-plan.nodes.begin());
            if (node == plan.nodes.end() || std::ranges::find(dependencies,position) != dependencies.end())
                return {{"status","invalid_plan"}};
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

    plan_ = std::move(plan);
    wait_for_work_ = true;
    publish("mc.partner.plan",{{"status","queued"},{"plan",plan_context()}});
    return {{"status","queued"},{"plan",plan_->id}};
}

// 返回可随新消息交付的图修订、替换关系和每个节点的原 ID。
Json PartnerLoop::plan_context() const {
    const auto& plan = *plan_;
    constexpr std::array statuses{"pending","running","done","failed","skipped"};
    Json nodes = Json::array();
    for (const auto& node : plan.nodes)
        nodes.push_back({{"id",node.id},{"skill",node.skill},{"layer",node.layer},{"work",node.work},
            {"status",statuses.at(static_cast<std::size_t>(node.state))}});
    const auto complete = std::ranges::all_of(plan.nodes,[](const auto& node) { return node.state == PlanNode::State::done; });
    return {{"id",plan.id},{"revision",plan.revision},{"supersedes",plan.supersedes},{"order",plan.order},
        {"status",plan.active ? "running" : plan.cancelled ? "cancelled" : complete ? "done" : "blocked"},{"nodes",std::move(nodes)}};
}

// 记录一次确切节点退出，以实际动作验证决定能否解除后续依赖。
void PartnerLoop::finish_plan_node(std::size_t index, bool ok, const Json& result) {
    auto& node = plan_->nodes.at(index);
    if (node.state != PlanNode::State::running) return;
    node.state = ok ? PlanNode::State::done : result.at("status") == "skipped" ? PlanNode::State::skipped : PlanNode::State::failed;
    node.result = plan_evidence(result);
}

// 停止未接纳节点并让已接纳的前后台动作进行真实取消。
// 步骤：标记计划取消 → 清除等待队列 → 保留在途动作直至读回。
void PartnerLoop::cancel_plan(std::string_view reason) {
    if (!plan_ || !plan_->active || plan_->cancelled) return;
    plan_->cancelled = true;
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
    if (closing || feed_.gap() || plan.revision != task_.revision()) cancel_plan("stale");

    const auto can_start = feed_.ready() && feed_.pending().empty() && (!request_ || order_ <= plan.order);
    Json results = Json::array();
    for (std::size_t index = 0; index < plan.nodes.size(); ++index) {
        auto& node = plan.nodes[index];
        if (node.layer != plan.layer) continue;
        if (node.state == State::pending && can_start) {
            const bool ready = std::ranges::all_of(node.after,[&](auto parent) { return plan.nodes[parent].state == State::done; });
            node.state = State::running;
            if (!ready) finish_plan_node(index,false,{{"status","skipped"},{"reason","prerequisite_failed"}});
            else if (node.skill == "craft") finish_plan_node(index,false,{{"status","blocked"},{"reasonCode","skill_unavailable"}});
            else if (node.skill == "observe") {
                const auto request = Json{{"context",{{"generation",session_world_},{"order",feed_.cursor()}}},
                    {"arguments",node.arguments}}.dump();
                node.reading = std::make_unique<GameWork>(GameAction::partner,config,node.work,request,"observe");
            } else {
                auto args = node.arguments;
                if (node.skill == "gather_ground") task_.bind_collection(args);
                const auto game_skill = node.skill == "fill_target" ? "fill_target" : "collect";
                queued_.push_back({plan.order,plan.request,plan.id,plan.revision,node.work,game_skill,std::move(args),{},true,false,index});
                pause_follow();
            }
        }
        if (node.reading) {
            std::optional<GameResult> read;
            try { read = node.reading->poll(plan.cancelled); }
            catch (...) { read = GameResult{false,GameEffect::unknown,{}}; }
            if (read) {
                const auto result = read->payload.empty() ? Json{{"status","unconfirmed"}} : Json::parse(read->payload);
                const Json fact{{"generation",session_world_},{"order",plan.order},{"session",session_},{"request",plan.request},
                    {"invocation",plan.id},{"bridgeWork",node.work},{"skill","observe"},{"effect",DFY_V2_TOOL_NOT_APPLIED},{"result",result}};
                if (!closing) task_.record(fact);
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
    publish("mc.partner.plan",layer);
    plan_receipts_.push_back(std::move(layer));
    ++plan.layer;
    plan.active = std::ranges::any_of(plan.nodes,[&](const auto& node) { return node.layer == plan.layer; });
}
}
