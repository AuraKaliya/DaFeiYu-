#include <dafeiyu/plugin_hooks.hpp>

namespace dfy::plugins {
// 将内部执行点及借用上下文投影为 ABI 值，复用 Agent 的拒绝与审批语义。
agent::PolicyExtension plugin_controls(PluginManager& manager, runtime::DomainId owner) {
    // 从原运行和工具身份构造 ABI 调用并转换插件结果。
    // 步骤：映射控制点 → 提交插件裁定 → 返回基础策略可合并的决定。
    return [&manager,owner](agent::ControlPoint point, const agent::PolicyContext& context) {
        using namespace dfy::agent;
        uint32_t hook_point = 0;
        switch (point) {
        case ControlPoint::model_request: hook_point = DFY_V2_HOOK_MODEL_REQUEST; break;
        case ControlPoint::tool_start: hook_point = DFY_V2_HOOK_TOOL_START; break;
        case ControlPoint::tool_result: hook_point = DFY_V2_HOOK_TOOL_RESULT; break;
        }
        const auto attempt = context.attempt;
        const dfy_v2_hook_input input{hook_point,0,attempt.step.run.session,attempt.step.run.sequence,
            attempt.step.sequence,attempt.sequence,owner,context.goal_revision,{context.tool_id.data(),context.tool_id.size()}};
        switch (manager.control_hook(input)) {
        case DFY_V2_HOOK_ALLOW: return PolicyDecision{PolicyVerdict::allow,PolicyReason::none};
        case DFY_V2_HOOK_CONFIRM: return PolicyDecision{PolicyVerdict::wait,PolicyReason::confirmation_required};
        case DFY_V2_HOOK_DENY: return PolicyDecision{PolicyVerdict::deny,PolicyReason::source_denied};
        }
        return PolicyDecision{PolicyVerdict::deny,PolicyReason::policy_failed};
    };
}
}
