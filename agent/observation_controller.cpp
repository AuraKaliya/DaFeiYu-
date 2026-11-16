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
}
// 仅准备有限规则和只读范围，不因为构造对象就自动启动运行。
// 步骤：固定宿主目标及运行选项 → 建立关闭的规则 → 拒绝非法目标、额外观察和状态变更范围。
ObservationController::ObservationController(Session& session, ObservationInbox& inbox, ObservationRule rule, std::string goal, RunOptions options)
    : session_(session),inbox_(inbox),options_(std::move(options)),goal_(std::move(goal)),trigger_(std::move(rule),initial_time(options_)) {
    if (goal_.empty() || goal_.size() > 64*1024 || goal_.find_first_not_of(" \t\r\n") == std::string::npos || !runtime::valid_utf8(goal_) ||
        !options_.observations.empty() || (options_.tools && options_.tools->permits_state_changes()))
        throw std::invalid_argument("observation controller: explicit goal and read-only scope required");
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
// 步骤：保护入口并处理在途停止 → 优先让出用户输入及忙碌会话 → 预留新采样 → 经正式 Session 接纳 → 固定回执或安全停止。
ObservationDispatch ObservationController::poll(bool user_pending) {
    using runtime::CancelReason; using runtime::OperationState;
    if (polling_) throw std::logic_error("observation controller: reentrant poll");
    polling_ = true; std::optional<TriggerTicket> reserved; std::shared_ptr<TurnRequest> accepted; ObservationTime started;
    try {
        started = options_.now(); const auto guard = trigger_.guard();
        if (const auto active = active_.lock(); active && active->exit()->state() == OperationState::pending) {
            auto decision = TriggerDecision::busy; const auto observation = active_observation_.lock();
            if (!guard(started)) {
                decision = started >= trigger_.rule().deadline ? TriggerDecision::expired : TriggerDecision::stopped;
                active->request_cancel(decision == TriggerDecision::expired ? CancelReason::deadline : CancelReason::owner_stop);
            } else if (observation && observation->validity(started) == ObservationValidity::source_revoked) {
                decision = TriggerDecision::stale; active->request_cancel(CancelReason::owner_stop);
            }
            polling_ = false; return {decision,active,{}};
        }
        const auto state = trigger_.availability(started);
        if (state != TriggerDecision::ready) { polling_ = false; return {state,{},{}}; }
        if (user_pending || session_.busy()) { polling_ = false; return {user_pending ? TriggerDecision::user_pending : TriggerDecision::busy,{},{}}; }
        const auto observation = inbox_.latest(trigger_.rule().source,trigger_.rule().object.id);
        if (!observation) { polling_ = false; return {TriggerDecision::missing_observation,{},{}}; }
        const auto offered = trigger_.reserve(*observation,started);
        if (!offered.ticket) { polling_ = false; return {offered.decision,{},{}}; }
        reserved = offered.ticket; auto options = options_; options.origin = RunOrigin::observation_rule;
        options.goal_revision = trigger_.rule().goal_revision; options.observations = {observation};
        options.deadline = std::min(trigger_.rule().deadline,options.deadline.value_or(trigger_.rule().deadline));
        // 在原规则回调后核对停止和来源撤销；采样到期仅约束初始接纳，行动前重观测属于 P03／A08。
        // 步骤：执行原控制规则 → 核对有限触发门 → 核对原来源仍有效 → 返回更严格决定。
        options.extension = [guard,observation,deadline = trigger_.rule().deadline,rule = std::move(options.extension)](auto point,const auto& context) {
            const auto decision = rule ? rule(point,context) : PolicyDecision{PolicyVerdict::allow,PolicyReason::none};
            if (!guard(context.now)) return PolicyDecision{PolicyVerdict::deny,context.now >= deadline ? PolicyReason::deadline : PolicyReason::cancelled};
            if (observation->validity(context.now) == ObservationValidity::source_revoked) return PolicyDecision{PolicyVerdict::deny,PolicyReason::source_denied};
            return decision;
        };
        accepted = session_.begin(goal_,std::move(options)); active_ = accepted; active_observation_ = observation;
        const auto admitted = options_.now(); const auto receipt = trigger_.commit(*reserved,accepted->run_id(),admitted); reserved.reset();
        if (!guard(admitted)) accepted->request_cancel(admitted >= trigger_.rule().deadline ? CancelReason::deadline : CancelReason::owner_stop);
        polling_ = false; return {TriggerDecision::ready,accepted,receipt};
    } catch (...) {
        if (reserved) {
            if (accepted) { trigger_.commit(*reserved,accepted->run_id(),started); stop(); }
            else trigger_.abort(*reserved);
        }
        polling_ = false; throw;
    }
}
}
