#include <dafeiyu/plugin_hooks.hpp>
#include <dafeiyu/session.hpp>

namespace dfy::plugins {
// 将内部执行点及借用上下文投影为 ABI 值，复用 Agent 的拒绝与审批语义。
agent::PolicyExtension plugin_controls(const PluginHookSnapshot& snapshot, runtime::DomainId owner) {
    // 从原运行和工具身份构造 ABI 调用并转换插件结果。
    // 步骤：映射控制点 → 提交插件裁定 → 返回基础策略可合并的决定。
    return [control = snapshot.control,owner](agent::ControlPoint point, const agent::PolicyContext& context) {
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
        switch (control(input)) {
        case DFY_V2_HOOK_ALLOW: return PolicyDecision{PolicyVerdict::allow,PolicyReason::none};
        case DFY_V2_HOOK_CONFIRM: return PolicyDecision{PolicyVerdict::wait,PolicyReason::confirmation_required};
        case DFY_V2_HOOK_DENY: return PolicyDecision{PolicyVerdict::deny,PolicyReason::source_denied};
        }
        return PolicyDecision{PolicyVerdict::deny,PolicyReason::policy_failed};
    };
}

// 把原运行的有界文本字段投影到原生插件契约，权限与回执留在 Agent。
agent::TextHook plugin_text_hooks(const PluginHookSnapshot& snapshot, runtime::DomainId owner) {
    // 映射运行阶段并将原字段借给插件管理器。
    // 步骤：定位 ABI 阶段 → 复制权威运行身份 → 合并已注册的字段改写。
    return [rewrite = snapshot.rewrite,owner](agent::TextHookPoint point, const agent::PolicyContext& context, std::string_view field,
        std::string_view value, std::size_t limit) {
        using agent::TextHookPoint;
        uint32_t hook_point = 0;
        switch (point) {
        case TextHookPoint::input: hook_point = DFY_V2_TEXT_HOOK_INPUT; break;
        case TextHookPoint::model_request: hook_point = DFY_V2_TEXT_HOOK_MODEL_REQUEST; break;
        case TextHookPoint::model_response: hook_point = DFY_V2_TEXT_HOOK_MODEL_RESPONSE; break;
        case TextHookPoint::tool_arguments: hook_point = DFY_V2_TEXT_HOOK_TOOL_ARGUMENTS; break;
        case TextHookPoint::reply: hook_point = DFY_V2_TEXT_HOOK_REPLY; break;
        }
        const auto attempt = context.attempt;
        return rewrite({hook_point,0,attempt.step.run.session,attempt.step.run.sequence,
            attempt.step.sequence,attempt.sequence,owner,context.goal_revision,{field.data(),field.size()},{value.data(),value.size()}},limit);
    };
}
// 通知本轮接纳时已经登记的退出观察者。
agent::TurnExitSink plugin_exit_hook(const PluginHookSnapshot& snapshot) {
    return [exited = snapshot.exited](const agent::TurnRequest& request) {
        const auto id = request.run_id();
        exited(id.session,id.sequence,static_cast<std::uint32_t>(request.operation()->state()));
    };
}
}
