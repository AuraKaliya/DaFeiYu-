#include <dafeiyu/execution_policy.hpp>
#include <iostream>
#include <stdexcept>

namespace {
using namespace dfy::agent;
using namespace dfy::runtime;
// 报告控制决定与期望约束不符。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 创建属于同一目标、策略和单调时钟的宿主上下文。
PolicyContext context() { return {AttemptId{StepId{RunId{1,1},1},1},1,1,RunOrigin::user_request,0,0,ToolTime{},ToolTime{}+std::chrono::seconds(1)}; }
// 验证模型请求边界由身份、版本、来源和预算共同控制。
// 步骤：通过正常请求 → 逐类拒绝过期上下文 → 验证限额和观察来源的明确选择。
void boundaries() {
    ExecutionPolicy policy({1,1},1); const auto original = context();
    check(policy.before_model(original).verdict == PolicyVerdict::allow,"valid model request denied");
    auto value = original; value.attempt.step.run.sequence = 2;
    check(policy.before_model(value).reason == PolicyReason::invalid_context,"foreign run accepted");
    value = original; value.goal_revision = 2; check(policy.before_model(value).reason == PolicyReason::stale_goal,"stale goal accepted");
    value = original; value.policy_revision = 2; check(policy.before_model(value).reason == PolicyReason::stale_policy,"stale policy accepted");
    value = original; value.now = value.deadline; check(policy.before_model(value).reason == PolicyReason::deadline,"expired request accepted");
    value = original; value.cancellation = CancelReason::user; check(policy.before_model(value).reason == PolicyReason::cancelled,"cancelled request accepted");
    value = original; value.model_requests = 16; check(policy.before_model(value).reason == PolicyReason::model_limit,"model limit ignored");
    value = original; value.origin = RunOrigin::observation_rule;
    check(policy.before_model(value).reason == PolicyReason::source_denied,"observation silently gained permission");
    PolicySettings settings; settings.allow_observation = true;
    check(ExecutionPolicy({1,1},1,settings).before_model(value).verdict == PolicyVerdict::allow,"explicit observation policy ignored");
}
// 验证确认只来自明确策略，结果后读取不会推进工作或改写业务状态。
// 步骤：解析实际注册目标 → 检查确认和限额 → 推进受控工作 → 检查终态和失效决定。
void tools() {
    ToolCatalog catalog;
    catalog.add({"fixture/change",3,1,{"change","fixture",R"({"type":"object","properties":{},"additionalProperties":false})"},true,false});
    const std::vector<std::string> ids{"fixture/change"}; const auto selected = catalog.snapshot(ids,true).resolve("change","{}");
    PolicySettings settings; settings.confirm_changes = true; ExecutionPolicy policy({1,1},1,settings); auto value = context();
    check(policy.before_tool(value,selected).reason == PolicyReason::confirmation_required,"confirmation policy ignored");
    check(ExecutionPolicy({1,1},1).before_tool(value,selected).verdict == PolicyVerdict::allow,"authorized work needlessly requires confirmation");
    value.tool_requests = 32; check(policy.before_tool(value,selected).reason == PolicyReason::tool_limit,"confirmation bypassed budget");
    unsigned calls = 0;
    ToolDriver driver({value.attempt,1,{"call","change","{}"}},[&](const auto&) {
        ++calls; return completed_tool({false,ToolEffect::applied,"partial",ToolError::failed});
    });
    const auto request = driver.request(); check(policy.after_tool(value,*request).reason == PolicyReason::work_pending && !calls,"policy advanced work");
    driver.advance();
    check(policy.after_tool(value,*request).verdict == PolicyVerdict::allow && calls == 1 && request->outcome().effect == ToolEffect::applied &&
        request->operation()->state() == OperationState::failed,"failed tool facts were dropped or budget blocked result receipt");
    value.cancellation = CancelReason::user;
    check(policy.after_tool(value,*request).verdict == PolicyVerdict::deny && request->operation()->state() == OperationState::failed,
        "policy decision rewrote original outcome");
}
// 验证显式控制回调异常和畸形决定都不会放行。
void rule_errors() {
    check(evaluate_policy({}).reason == PolicyReason::policy_failed,"empty rule allowed");
    check(evaluate_policy([]() -> PolicyDecision { throw std::runtime_error("fixture"); }).verdict == PolicyVerdict::deny,"throwing rule allowed");
    check(evaluate_policy([] { return PolicyDecision{PolicyVerdict::allow,PolicyReason::deadline}; }).verdict == PolicyVerdict::deny,"malformed allow accepted");
    check(evaluate_policy([] { return PolicyDecision{PolicyVerdict::wait,PolicyReason::confirmation_required}; }).verdict == PolicyVerdict::wait,"valid wait changed");
    unsigned called = 0;
    const PolicyExtension allow = [&](auto,const auto&) { ++called; return PolicyDecision{PolicyVerdict::allow,PolicyReason::none}; };
    check(restrict_policy({PolicyVerdict::deny,PolicyReason::tool_limit},allow,ControlPoint::tool_start,context()).reason == PolicyReason::tool_limit && !called,
        "extension ran after denial or expanded permission");
    check(restrict_policy({PolicyVerdict::wait,PolicyReason::confirmation_required},allow,ControlPoint::tool_start,context()).verdict == PolicyVerdict::wait,
        "extension bypassed confirmation");
    check(restrict_policy({PolicyVerdict::allow,PolicyReason::none},[](auto,const auto&) -> PolicyDecision { throw std::runtime_error("fixture"); },
        ControlPoint::model_request,context()).reason == PolicyReason::policy_failed,"throwing extension allowed");
}
}
// 只验证控制规则与固定工具工作，不调用模型或 UI。
int main() {
    try { boundaries(); tools(); rule_errors(); std::cout << "Execution policy checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
