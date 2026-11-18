#include <dafeiyu/observation_controller.hpp>
#include <dafeiyu/utf8.hpp>
#include <algorithm>
#include <stdexcept>

namespace dfy::agent {
namespace {
// 构造规则前读取明确宿主时钟，缺少时钟直接拒绝。
ObservationTime initial_time(const RunOptions& options) {
    if (!options.now) throw std::invalid_argument("observation controller: clock required");
    return options.now();
}
struct PollReset {
    bool& polling;
    // 无论正常返回还是接纳失败，都归还本轮轮询资格。
    ~PollReset() { polling = false; }
};
}
// 准备有限观察规则和显式授权，待宿主开启后才能接纳运行。
// 步骤：固定宿主参数 → 校验目标和范围 → 核对唯一变更工具、明确条件及确认路径。
ObservationController::ObservationController(Session& session, ObservationInbox& inbox, ObservationRule rule, std::string goal, RunOptions options,
    std::optional<ObservationAction> action)
    : session_(session),inbox_(inbox),options_(std::move(options)),goal_(std::move(goal)),action_(std::move(action)),
      trigger_(std::move(rule),initial_time(options_)) {
    const bool changes = options_.tools && options_.tools->permits_state_changes();
    if (goal_.empty() || goal_.size() > 64*1024 || goal_.find_first_not_of(" \t\r\n") == std::string::npos || !runtime::valid_utf8(goal_) ||
        !options_.observations.empty() || changes != action_.has_value() || options_.observation_action)
        throw std::invalid_argument("observation controller: explicit goal and action scope required");
    if (!action_) return;

    const auto tools = options_.tools->descriptions(false);
    if (tools.size() != 1 || tools.front().id != action_->tool_id || !options_.policy.confirm_changes ||
        !options_.confirmation_caller || options_.planning || options_.action_observations ||
        !options_.policy.tool_limit || options_.policy.tool_limit > 64 || !options_.confirmation_limit || options_.confirmation_limit > 64)
        throw std::invalid_argument("observation controller: one confirmed action required");
    ObservationExpectation::validate_condition(action_->field,action_->expected);
}
// 不接管会话销毁，只停止本规则及其已经接纳的运行。
ObservationController::~ObservationController() { stop(); }
// 开启仍服从独立运行策略，不能通过触发规则绕过来源授权。
void ObservationController::enable() {
    if (!options_.policy.allow_observation) throw std::invalid_argument("observation controller: origin not authorized");
    trigger_.enable();
}
// 只取消自己关联的原运行，外部持有和后台清理仍继续有效。
void ObservationController::stop() noexcept {
    trigger_.stop();
    if (const auto request = active_.lock()) request->request_cancel(runtime::CancelReason::owner_stop);
}
// 按一个宿主节拍处理接纳，失败撤回预留，真实接纳不因后续时钟错误漏计。
// 步骤：复核原行动结算 → 让出用户及在途工作 → 核对采样和确定条件 → 由正式 Session 接纳并记账。
ObservationDispatch ObservationController::poll(bool user_pending) {
    using runtime::CancelReason; using runtime::OperationState;
    if (polling_) throw std::logic_error("observation controller: reentrant poll");
    polling_ = true; const PollReset reset{polling_};
    std::optional<TriggerTicket> reserved; std::shared_ptr<TurnRequest> accepted; ObservationTime started;
    try {
        if (action_run_ && action_run_->exit()->state() != OperationState::pending) {
            auto finished = std::move(action_run_);
            if (finished->operation()->state() != OperationState::succeeded) {
                trigger_.stop();
                return {TriggerDecision::stopped,std::move(finished),{}};
            }
        }
        started = options_.now(); const auto guard = trigger_.guard();
        if (const auto active = active_.lock(); active && active->exit()->state() == OperationState::pending) {
            auto decision = TriggerDecision::busy; const auto observation = active_observation_.lock();
            if (!guard(started)) {
                decision = started >= trigger_.rule().deadline ? TriggerDecision::expired : TriggerDecision::stopped;
                active->request_cancel(decision == TriggerDecision::expired ? CancelReason::deadline : CancelReason::owner_stop);
            } else if (observation && observation->validity(started) == ObservationValidity::source_revoked) {
                decision = TriggerDecision::stale; active->request_cancel(CancelReason::owner_stop);
            }
            return {decision,active,{}};
        }
        const auto state = trigger_.availability(started);
        if (state != TriggerDecision::ready) return {state,{},{}};
        if (user_pending || session_.busy()) return {user_pending ? TriggerDecision::user_pending : TriggerDecision::busy,{},{}};
        const auto observation = inbox_.latest(trigger_.rule().source,trigger_.rule().object.id);
        if (!observation) return {TriggerDecision::missing_observation,{},{}};
        auto options = options_;
        if (action_) {
            const auto available = trigger_.inspect(*observation,started);
            if (available != TriggerDecision::ready) return {available,{},{}};
            const auto sample = inbox_.guard(observation,started);
            const auto outcome = ObservationExpectation(sample,action_->field,action_->expected).assess(sample,started).match;
            if (outcome != ObservationMatch::satisfied)
                return {outcome == ObservationMatch::not_satisfied ? TriggerDecision::condition_not_met : TriggerDecision::condition_unknown,{},{}};
            options.action_observations.emplace(std::vector<ObservationGuard>{sample});
            options.observation_action = ObservationActionAuthorization(options.action_observations->binding(),trigger_.rule().revision);
            options.policy.tool_limit = options.confirmation_limit = 1;
        }

        const auto offered = trigger_.reserve(*observation,started);
        if (!offered.ticket) return {offered.decision,{},{}};
        reserved = offered.ticket; options.origin = RunOrigin::observation_rule;
        options.goal_revision = trigger_.rule().goal_revision; options.observations = {observation};
        options.deadline = std::min(trigger_.rule().deadline,options.deadline.value_or(trigger_.rule().deadline));
        if (!options.extension && !options.version_labels.control_extension)
            options.version_labels.control_extension = action_ ? "agent.observation-action-v1" : "agent.observation-controller-v1";
        // 在原规则回调后核对停止和来源撤销；采样到期仅约束初始接纳，行动前重观测属于 P03／A08。
        // 步骤：执行原控制规则 → 核对有限触发门 → 核对原来源仍有效 → 返回更严格决定。
        options.extension = [guard,observation,deadline = trigger_.rule().deadline,rule = std::move(options.extension)](auto point,const auto& context) {
            const auto decision = rule ? rule(point,context) : PolicyDecision{PolicyVerdict::allow,PolicyReason::none};
            if (!guard(context.now)) return PolicyDecision{PolicyVerdict::deny,context.now >= deadline ? PolicyReason::deadline : PolicyReason::cancelled};
            if (observation->validity(context.now) == ObservationValidity::source_revoked) return PolicyDecision{PolicyVerdict::deny,PolicyReason::source_denied};
            return decision;
        };
        accepted = session_.begin(goal_,std::move(options)); active_ = accepted; active_observation_ = observation;
        if (action_) action_run_ = accepted;
        const auto admitted = options_.now(); const auto receipt = trigger_.commit(*reserved,accepted->run_id(),admitted); reserved.reset();
        if (!guard(admitted)) accepted->request_cancel(admitted >= trigger_.rule().deadline ? CancelReason::deadline : CancelReason::owner_stop);
        return {TriggerDecision::ready,accepted,receipt};
    } catch (...) {
        if (reserved) {
            if (accepted) { trigger_.commit(*reserved,accepted->run_id(),started); stop(); }
            else { trigger_.abort(*reserved); if (action_) trigger_.stop(); }
        }
        throw;
    }
}
}
