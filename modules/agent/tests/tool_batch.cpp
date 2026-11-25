#include <dafeiyu/tool_batch.hpp>
#include <iostream>

namespace {
using namespace dfy::agent;
using namespace dfy::runtime;
using dfy::model::Role;
// 报告受控批次与上下文事实不一致。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 创建宿主控制上下文，不从模型输出读取权限或目标身份。
PolicyContext control() { return {AttemptId{StepId{RunId{1,1},1},1},1,1,RunOrigin::user_request,1,0,ToolTime{},ToolTime{}+std::chrono::seconds(1)}; }
// 登记计数工具，只有实际工厂调用才改变测试状态。
void register_tool(ToolCatalog& catalog, unsigned& calls) {
    catalog.add({"fixture/change",1,1,{"change","fixture",R"({"type":"object","properties":{},"additionalProperties":false})"},true,false},
        [&](const auto&) { ++calls; return completed_tool({true,ToolEffect::applied,"changed",ToolError::none}); });
}
// 验证合法与拒绝候选均按源顺序回填，准备不执行，重复推进不重放。
void mixed() {
    unsigned calls = 0; ToolCatalog catalog; register_tool(catalog,calls);
    const std::vector<std::string> ids{"fixture/change"}; const auto tools = catalog.snapshot(ids,true);
    ToolExecutor executor({1,1},tools); RunContext context(std::make_shared<const MessageHistory>(),"goal");
    ToolBatch batch(context,executor,{Role::assistant,"",{{"one","change","{}"},{"two","missing","{}"}}},tools,ExecutionPolicy({1,1},1),control());
    check(!calls && !batch.done(),"batch preparation executed tool");
    check(batch.advance(control()) && calls == 1 && batch.advance(control()) && calls == 1,"batch replayed action");
    const auto facts = batch.snapshot(); const auto history = context.committed();
    check(facts[0].accepted && facts[0].effect == ToolEffect::applied && !facts[1].accepted && history->size() == 4 &&
        (*history)[2].tool_call_id == "one" && (*history)[3].content.starts_with("status=rejected"),"batch results lost order or effects");
}
// 验证确认要求、规则异常和接纳后的取消都不能启动未授权动作。
// 步骤：逐类建立受控批次 → 推进明确拒绝或取消 → 验证零动作和配对结果。
void denials() {
    for (unsigned mode = 0; mode < 3; ++mode) {
        unsigned calls = 0; ToolCatalog catalog; register_tool(catalog,calls); const std::vector<std::string> ids{"fixture/change"};
        const auto tools = catalog.snapshot(ids,true); ToolExecutor executor({1,1},tools);
        RunContext context(std::make_shared<const MessageHistory>(),"goal"); PolicySettings settings; settings.confirm_changes = mode == 0;
        PolicyExtension extension;
        if (mode == 1) extension = [](auto,const auto&) -> PolicyDecision { throw std::runtime_error("controlled"); };
        ToolBatch batch(context,executor,{Role::assistant,"",{{"one","change","{}"}}},tools,ExecutionPolicy({1,1},1,settings),control(),extension);
        auto current = control(); if (mode == 2) current.cancellation = CancelReason::user;
        check(batch.advance(current) && !calls && context.committed()->size() == 3,"denied or cancelled candidate executed");
    }
}
// 验证结果后策略拒绝不抹去已发生动作，模型后续调度得到明确停止决定。
void after_result() {
    unsigned calls = 0; ToolCatalog catalog; register_tool(catalog,calls);
    catalog.add({"fixture/other",2,1,{"other","fixture",R"({"type":"object","properties":{},"additionalProperties":false})"},true,false},
        [&](const auto&) { ++calls; return completed_tool({true,ToolEffect::applied,"other",ToolError::none}); });
    const std::vector<std::string> ids{"fixture/change","fixture/other"};
    const auto tools = catalog.snapshot(ids,true); ToolExecutor executor({1,1},tools); RunContext context(std::make_shared<const MessageHistory>(),"goal");
    PolicyExtension extension = [](auto point,const auto&) { return point == ControlPoint::tool_result ? PolicyDecision{PolicyVerdict::deny,PolicyReason::policy_failed} : PolicyDecision{PolicyVerdict::allow,PolicyReason::none}; };
    ToolBatch batch(context,executor,{Role::assistant,"",{{"one","change","{}"},{"two","other","{}"}}},tools,ExecutionPolicy({1,1},1),control(),extension);
    check(!batch.advance(control()) && batch.advance(control()) && calls == 1 && batch.continuation().verdict == PolicyVerdict::deny &&
        (*context.committed())[2].content.find("effect=applied") != std::string::npos && batch.snapshot()[1].result == OperationState::cancelled,
        "post-result denial erased effects or allowed a later action");
}
}
// 验证固定候选工具批次，不调用模型。
int main() {
    try { mixed(); denials(); after_result(); std::cout << "Tool batch checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
