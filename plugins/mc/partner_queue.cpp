#include "partner_loop.hpp"
#include "partner_effect.hpp"
#include <algorithm>
#include <iterator>

namespace dfy::mc {
namespace {
using Json = nlohmann::json;
constexpr std::size_t queue_limit = 4;

// 判断步骤是否只携带一种坐标目标及对应的已支持参数。
// 步骤：核对移动与跟随的独立参数 → 校验精确方块及采集数量。
bool valid_step(std::string_view skill, const Json& args) {
    if (!args.is_object() || args.dump().size() > 512) return false;
    if (skill == "follow") return args.empty() || (args.size() == 1 && args.contains("distance") &&
        args.at("distance").is_number_integer() && args.at("distance") >= 2 && args.at("distance") <= 5);
    if (skill == "move" && args.size() == 1) return (args.contains("place") && args.at("place").is_string()) ||
        (args.contains("message") && args.at("message").is_number_unsigned() && args.at("message") > 0);
    for (const auto axis : {"x","y","z"}) {
        if (!args.contains(axis) || !args.at(axis).is_number_integer() ||
            args.at(axis) < -30000000 || args.at(axis) > 30000000) return false;
    }
    if (skill == "move") return args.size() == 3;
    if (!args.contains("block") || !args.at("block").is_string()) return false;
    if (skill == "place") return args.size() == 4;
    return skill == "collect" && args.size() == 5 && args.contains("count") && args.at("count") == 1;
}
}

// 接纳带修订的模型意图，替换时只有全部新步骤通过才能停止旧工作。
// 步骤：核对操作与旧工作 → 校验本轮工具范围及步骤参数 → 更新待执行队列。
nlohmann::json PartnerLoop::queue_actions(std::uint64_t invocation, const Json& args) {
    const auto operation = args.at("operation").get<std::string>();
    const auto resolves = args.value("resolves",std::uint64_t{});
    if (resolves && operation != "plan") return {{"status","invalid_plan"},{"reason","plan_operation_required"}};
    if (operation != "clear") {
        if (const auto reason = front_barrier(session_,operation == "plan" ? "route" : ""); !reason.empty())
            return {{"status","blocked"},{"reason",reason}};
    }
    if (waits_for_player(session_,resolves) && operation != "clear") return {{"status","blocked"},{"reason","waiting_for_player"}};
    if (operation == "plan") return submit_plan(invocation,args);
    if (operation == "clear") {
        cancel_plan("cleared");
        clear_queue("cleared");
        return {{"status","cleared"}};
    }
    if (plan_ && plan_->active) return {{"status","busy"}};
    const bool replace = operation == "replace";
    const bool interrupt = operation == "interrupt";
    if ((!replace && !interrupt && operation != "append") ||
        (interrupt && (!follow_ || (motion_ && motion_->skill == "follow" && motion_->stopping && !follow_->pausing))) ||
        (replace && (!motion_ || !args.contains("work") || (args.at("work") != motion_->work &&
            (!follow_ || motion_->skill != "follow" || args.at("work") != follow_->id)))))
        return {{"status","stale"}};

    const auto& steps = args.at("steps");
    const auto waiting = replace ? 0 : queued_.size();
    if (!steps.is_array() || steps.empty() || waiting+steps.size() > queue_limit)
        throw std::invalid_argument("MC queue: expected one to four steps");
    std::deque<QueuedAction> pending;
    Json ids = Json::array();
    const auto enabled = scope();
    for (const auto& step : steps) {
        if (step.contains("id") || step.contains("after")) return {{"status","invalid_plan"},{"reason","plan_operation_required"}};
        const auto name = step.at("skill").get<std::string>();
        const auto& parameters = step.at("arguments");
        if (name == "collect" && requires_pointed_dig(session_))
            return {{"status","blocked"},{"reason","pointed_block_unverified"}};
        const auto pickup = step.value("pickup",std::string{});
        const auto required = item_name(pickup);
        if (step.contains("pickup") && (name != "collect" || required.empty()))
            throw std::invalid_argument("MC queue: invalid required pickup");
        const auto skill = std::ranges::find(partner_skills,name,&PartnerSkill::name);
        const auto* action = partner_action(name);
        if (!action || !action->sequence || skill == partner_skills.end() || (interrupt && name == "follow") || !valid_step(name,parameters) ||
            (skill->category != SkillCategory::movement && !enabled.test(static_cast<std::size_t>(skill->category))) ||
            (name == "place" && !task_.target_ready()) || requires_approval(name,parameters))
            throw std::invalid_argument("MC queue: step is outside the current action scope");
        auto id = "queued."+std::to_string(invocation)+"."+std::to_string(pending.size()+1);
        ids.push_back(id);
        pending.push_back({order_,request_,invocation,task_.revision(),std::move(id),action->executor,parameters,std::string(required),interrupt});
    }

    if (replace) {
        clear_queue("replaced");
        if (follow_) follow_->pausing = false;
        if (motion_->skill != "follow") follow_.reset();
        motion_->stopping = true;
        replacing_motion_ = true;
    }
    const auto position = interrupt ? std::ranges::find(queued_,false,&QueuedAction::interrupt) : queued_.end();
    queued_.insert(position,std::make_move_iterator(pending.begin()),std::make_move_iterator(pending.end()));
    if (interrupt) pause_follow();
    publish("mc.partner.queue",{{"status","queued"},{"works",ids},{"revision",task_.revision()},{"order",order_}});
    return {{"status","queued"},{"works",std::move(ids)}};
}

// 保存直接动作并请求跟随释放角色，原任务与普通排队项继续保留。
// 步骤：检查在途行为和回合 → 验证收件身份 → 登记优先动作并暂停。
bool PartnerLoop::interrupt_follow(std::uint64_t session, std::uint64_t invocation, const std::string& work,
    std::string_view skill, std::string_view envelope, bool wait, bool pointed) {
    if ((plan_ && plan_->active) || !follow_ || session != session_ || !request_ || delegated(session) || !permits(session) ||
        !feed_.ready() || feed_.departed() || queued_.size() >= queue_limit ||
        (motion_ && motion_->skill == "follow" && motion_->stopping && !follow_->pausing)) return false;
    const auto input = Json::parse(envelope);
    const auto& context = input.at("context");
    if (context.at("generation") != session_world_ || context.at("order") != order_) return false;
    const auto position = std::ranges::find(queued_,false,&QueuedAction::interrupt);
    QueuedAction action{order_,request_,invocation,task_.revision(),work,skill,input.at("arguments"),{},true,wait};
    action.pointed = pointed;
    queued_.insert(position,std::move(action));
    pause_follow();
    wait_for_work_ = wait_for_work_ || wait;
    return true;
}

// 请求结束跟随当前执行段，后续按实际退出决定能否交接。
void PartnerLoop::pause_follow() noexcept {
    if (follow_ && motion_ && motion_->skill == "follow" && !motion_->stopping) {
        follow_->pausing = true;
        motion_->stopping = true;
    }
}

// 恢复同一持续意图，使用最新游标和新桥接工作号。
// 步骤：核对当前绑定和前台占用 → 构造本段工作 → 发布原跟随身份。
void PartnerLoop::resume_follow(const BridgeConfig& config, bool body_busy) {
    if (!follow_ || !follow_->suspended || motion_ || body_busy || !feed_.ready() || feed_.gap() ||
        !feed_.pending().empty() || (!queued_.empty() && queued_.front().interrupt) || awaiting_restarted_route_ ||
        (handoff_ && (handoff_->receipt.is_null() || handoff_->receipt.at("status") != "exited")) ||
        (task_.navigation() && plan_ && plan_->active && plan_->revision == task_.revision())) return;
    auto& follow = *follow_;
    const auto work = follow.id+".resume."+std::to_string(follow.segment+1);
    const auto envelope = Json{{"context",{{"generation",session_world_},{"order",feed_.cursor()}}},
        {"arguments",follow.arguments}}.dump();
    auto action = std::make_unique<GameWork>(GameAction::partner,config,work,envelope,"follow");
    motion_.emplace(Motion{follow.order,session_,follow.request,follow.invocation,session_world_,work,"follow",std::move(action)});
    motion_->accepted = false;
    ++follow.segment;
    follow.suspended = false;
}

// 结算指定调用组或全部尚未发送给游戏的步骤。
void PartnerLoop::clear_queue(std::string_view reason, std::uint64_t group) {
    Json ids = Json::array();
    std::erase_if(queued_,[&](const auto& step) {
        if (group && step.invocation != group) return false;
        ids.push_back(step.work);
        if (step.plan_node) finish_plan_node(*step.plan_node,false,{{"status","skipped"},{"reason",reason}});
        else if (step.notify || std::ranges::find(settlements_,step.invocation,&WorkSettlement::invocation) != settlements_.end())
            settle_work(step.invocation,{{"invocation",step.invocation},{"skill",step.skill},{"bridgeWork",step.work},
                {"result",{{"status","skipped"},{"reason",reason}}}});
        return true;
    });
    if (!ids.empty()) publish("mc.partner.queue",{{"status",reason},{"works",std::move(ids)}});
}

// 将队首变为唯一占用角色的游戏工作，原输入和实际接纳游标分别保留。
// 步骤：处理配对和修订失效 → 等待角色控制权 → 重验动作并交给桥接。
void PartnerLoop::advance_queue(const BridgeConfig& config, bool closing, bool body_busy) {
    if (queued_.empty()) { if (!closing) resume_follow(config,body_busy); return; }
    const auto& step = queued_.front();
    if (closing || feed_.gap()) { clear_queue("stale"); return; }
    if (step.revision != task_.revision()) { clear_queue("stale",step.invocation); return; }
    if (follow_ && follow_->suspended && !step.interrupt) { resume_follow(config,body_busy); return; }
    if (step.plan_node && request_ && order_ > step.order) return;
    if (body_busy || motion_ || !feed_.ready() || !feed_.pending().empty()) return;
    auto args = step.arguments;
    if (requires_approval(step.skill,args) || (step.skill == "place" && !task_.bind_placement(args)) ||
        (step.skill == "fill_target" && !bind_fill(args))) {
        if (step.plan_node) {
            finish_plan_node(*step.plan_node,false,{{"status","blocked"},{"reason","scope_or_approval"}});
            queued_.pop_front();
        } else clear_queue("blocked",step.invocation);
        return;
    }
    // 直接指定单格的挖掘已固定范围，普通队列采集继承任务的取材约束。
    if (step.skill == "collect" && !args.contains("source")) task_.bind_collection(args);
    const auto envelope = Json{{"context",{{"generation",session_world_},{"order",feed_.cursor()}}},
        {"arguments",std::move(args)}}.dump();
    auto action = std::make_unique<GameWork>(GameAction::partner,config,step.work,envelope,std::string(step.skill));
    if (step.skill == "follow") follow_ = Follow{step.order,step.request,step.invocation,1,step.work,step.arguments};
    motion_.emplace(Motion{step.order,session_,step.request,step.invocation,session_world_,step.work,step.skill,std::move(action)});
    motion_->accepted = false;
    motion_->queued = true;
    motion_->task = task_.id();
    motion_->revision = step.revision;
    motion_->pickup = step.pickup;
    motion_->plan_node = step.plan_node;
    motion_->pointed = step.pointed;
    queued_.pop_front();
}
}
