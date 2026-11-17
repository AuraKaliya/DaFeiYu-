#include <dafeiyu/execution_policy.hpp>
#include <stdexcept>

namespace dfy::agent {
// 校验运行、修订和限额后保存不可变策略，不执行外部规则。
ExecutionPolicy::ExecutionPolicy(RunId run, std::uint64_t goal_revision, PolicySettings settings)
    : run_(run), goal_revision_(goal_revision), settings_(settings) {
    if (!run.session || !run.sequence || !goal_revision || !settings.revision || !settings.model_limit || settings.model_limit > 64 ||
        !settings.tool_limit || settings.tool_limit > 64) throw std::invalid_argument("policy: invalid binding or limits");
}
// 从宿主值上下文裁定是否可继续，不把外部数据来源升级为用户授权。
// 步骤：校验身份及枚举 → 核对目标和策略版本 → 按取消、期限及来源分流。
PolicyDecision ExecutionPolicy::common(const PolicyContext& context) const noexcept {
    using runtime::CancelReason;
    if (context.attempt.step.run != run_ || !context.attempt.step.sequence || !context.attempt.sequence ||
        (context.origin != RunOrigin::user_request && context.origin != RunOrigin::observation_rule) ||
        context.plan_revision > 16 || context.plan_step > 32 || (static_cast<bool>(context.plan_revision) != static_cast<bool>(context.plan_step)) ||
        (context.cancellation != CancelReason::none && context.cancellation != CancelReason::user &&
         context.cancellation != CancelReason::owner_stop && context.cancellation != CancelReason::deadline))
        return {PolicyVerdict::deny,PolicyReason::invalid_context};
    if (context.goal_revision != goal_revision_) return {PolicyVerdict::deny,PolicyReason::stale_goal};
    if (context.policy_revision != settings_.revision) return {PolicyVerdict::deny,PolicyReason::stale_policy};
    if (context.cancellation != CancelReason::none) return {PolicyVerdict::deny,PolicyReason::cancelled};
    if (context.now >= context.deadline) return {PolicyVerdict::deny,PolicyReason::deadline};
    if (context.origin == RunOrigin::observation_rule && !settings_.allow_observation) return {PolicyVerdict::deny,PolicyReason::source_denied};
    return {PolicyVerdict::allow,PolicyReason::none};
}
// 在共同边界通过后核对模型请求次数，不改变已消耗预算。
PolicyDecision ExecutionPolicy::before_model(const PolicyContext& context) const noexcept {
    const auto base = common(context); if (base.verdict != PolicyVerdict::allow) return base;
    if (context.model_requests >= settings_.model_limit) return {PolicyVerdict::deny,PolicyReason::model_limit};
    return base;
}
// 在明确范围内决定工具是否需要确认，确认本身不能绕过目录或参数检查。
// 步骤：校验运行控制边界 → 核对候选身份及调用预算 → 根据明确策略等待或放行。
PolicyDecision ExecutionPolicy::before_tool(const PolicyContext& context, const ToolSelection& tool) const noexcept {
    const auto base = common(context); if (base.verdict != PolicyVerdict::allow) return base;
    if (!tool.registration || !tool.description.instance || !tool.description.version || tool.description.id.empty())
        return {PolicyVerdict::deny,PolicyReason::invalid_context};
    if (tool.description.changes_state && !context.observations_current) return {PolicyVerdict::deny,PolicyReason::invalid_context};
    if (context.tool_requests >= settings_.tool_limit) return {PolicyVerdict::deny,PolicyReason::tool_limit};
    if (tool.description.changes_state && settings_.confirm_changes) return {PolicyVerdict::wait,PolicyReason::confirmation_required};
    return base;
}
// 先保留权威回执，只有原工作真实退出后才允许协调器继续后续步骤。
// 步骤：核对原调用身份 → 裁定停止边界 → 等待实际退出或允许接收终态。
PolicyDecision ExecutionPolicy::after_tool(const PolicyContext& context, const ToolRequest& request) const noexcept {
    if (request.invocation().attempt != context.attempt) return {PolicyVerdict::deny,PolicyReason::invalid_context};
    const auto base = common(context); if (base.verdict != PolicyVerdict::allow) return base;
    if (request.exit()->state() == runtime::OperationState::pending) return {PolicyVerdict::wait,PolicyReason::work_pending};
    if (request.operation()->state() == runtime::OperationState::pending) return {PolicyVerdict::deny,PolicyReason::invalid_result};
    return base;
}
// 在显式控制边界隔离规则异常，拒绝不合法的决定组合。
// 步骤：检查规则 → 执行并核对决定 → 按返回值或异常给出固定拒绝原因。
PolicyDecision evaluate_policy(const std::function<PolicyDecision()>& rule) noexcept {
    try {
        if (!rule) return {PolicyVerdict::deny,PolicyReason::policy_failed};
        const auto decision = rule();
        if (decision.verdict == PolicyVerdict::allow && decision.reason == PolicyReason::none) return decision;
        if (decision.verdict == PolicyVerdict::wait &&
            (decision.reason == PolicyReason::confirmation_required || decision.reason == PolicyReason::work_pending)) return decision;
        if (decision.verdict == PolicyVerdict::deny && decision.reason != PolicyReason::none &&
            decision.reason != PolicyReason::confirmation_required && decision.reason != PolicyReason::work_pending &&
            static_cast<unsigned>(decision.reason) <= static_cast<unsigned>(PolicyReason::policy_failed)) return decision;
    } catch (...) {}
    return {PolicyVerdict::deny,PolicyReason::policy_failed};
}
// 对基础策略作单向收紧，不允许扩展重新授予已拒绝的权限。
// 步骤：核对基础决定 → 跳过已拒绝或空扩展 → 隔离扩展异常 → 保留更严格结果。
PolicyDecision restrict_policy(PolicyDecision base, const PolicyExtension& extension, ControlPoint point, const PolicyContext& context) noexcept {
    try {
        base = evaluate_policy([base] { return base; });
        if (base.verdict == PolicyVerdict::deny || !extension) return base;
        const auto extra = evaluate_policy([&] { return extension(point,context); });
        if (extra.verdict == PolicyVerdict::deny || base.verdict == PolicyVerdict::allow) return extra;
        return base;
    } catch (...) { return {PolicyVerdict::deny,PolicyReason::policy_failed}; }
}
}
