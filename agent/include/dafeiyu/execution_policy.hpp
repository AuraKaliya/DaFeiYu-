#pragma once
#include <dafeiyu/tool_catalog.hpp>

namespace dfy::agent {
enum class RunOrigin { user_request, observation_rule };
enum class PolicyVerdict { allow, deny, wait };
enum class PolicyReason {
    none, invalid_context, stale_goal, stale_policy, cancelled, deadline,
    model_limit, tool_limit, source_denied, confirmation_required, work_pending, invalid_result, policy_failed
};
struct PolicyDecision { PolicyVerdict verdict = PolicyVerdict::deny; PolicyReason reason = PolicyReason::invalid_context; };
// 由宿主从权威运行状态生成的值上下文，模型文本和普通观察通知不能授予这些字段。
struct PolicyContext {
    AttemptId attempt;
    std::uint64_t goal_revision = 0, policy_revision = 0;
    RunOrigin origin = RunOrigin::user_request;
    std::size_t model_requests = 0, tool_requests = 0;
    ToolTime now, deadline;
    runtime::CancelReason cancellation = runtime::CancelReason::none;
};
struct PolicySettings {
    std::uint64_t revision = 1;
    std::size_t model_limit = 16, tool_limit = 32;
    bool allow_observation = false, confirm_changes = false;
};
// 固定运行和目标版本的纯控制规则，不拥有业务终态或替代工具入口的最终权限校验。
class ExecutionPolicy {
  public:
    // 校验并固定策略范围，限额不超过当前有界执行器支持的规模。
    ExecutionPolicy(RunId run, std::uint64_t goal_revision, PolicySettings settings = {});
    // 判断能否提交新的模型请求，不发起模型调用或更新计数。
    PolicyDecision before_model(const PolicyContext& context) const noexcept;
    // 对已解析候选工具增加策略检查，允许决定不能替代执行入口的再次授权。
    PolicyDecision before_tool(const PolicyContext& context, const ToolSelection& tool) const noexcept;
    // 只约束后续调度；结果和效果须先记录，任何决定都不能伪造工作退出。
    PolicyDecision after_tool(const PolicyContext& context, const ToolRequest& request) const noexcept;
    // 返回固定策略版本和限额副本，不允许调用方原位扩权。
    PolicySettings settings() const noexcept { return settings_; }
  private:
    // 核对身份、修订、来源、取消和期限，保持固定的拒绝优先级。
    PolicyDecision common(const PolicyContext& context) const noexcept;
    const RunId run_;
    const std::uint64_t goal_revision_;
    const PolicySettings settings_;
};
// 显式控制规则须只作决定，不执行业务；异常、空规则或畸形决定均拒绝，不处理普通观察者。
PolicyDecision evaluate_policy(const std::function<PolicyDecision()>& rule) noexcept;
}
