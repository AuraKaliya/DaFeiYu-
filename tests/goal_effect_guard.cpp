#include "support/scripted_model.hpp"
#include <dafeiyu/session.hpp>
#include <iostream>
#include <thread>

namespace {
using namespace dfy::agent;
using namespace dfy::runtime;
using namespace dfy::model;
using namespace dfy::tests;
// 报告新目标或新批准重复施加不确定动作。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 有界推进到原运行的明确状态。
void until(Session& session, const auto& ready) {
    const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (!ready() && std::chrono::steady_clock::now() < end) { session.advance(); std::this_thread::yield(); }
    check(ready(),"effect guard scenario stalled");
}
// 用新目标和新批准重复同一非幂等候选，未知或失败已发生的旧效果必须继续阻止调用。
// 步骤：执行一次不确定动作 → 保留原回执并修改目标 → 批准新候选 → 核对零重复调用及旧效果。
void repeated(ToolEffect effect) {
    unsigned calls = 0; ToolCatalog catalog;
    catalog.add({"fixture/change",1,1,{"change","fixture",R"({"type":"object","properties":{},"additionalProperties":false})"},true,false},
        [&](const auto&) { ++calls; return completed_tool({false,effect,"partial",ToolError::failed}); });
    ScriptStep generation; generation.response = Response{{Role::assistant,R"({"schema_version":1,"steps":[{"id":"change","objective":"change once","completion_criterion":"host checks","tools":["fixture/change"]}]})"}};
    ScriptStep action; action.tool_names = {"change"}; action.response = Response{{Role::assistant,"",{{"call","change","{}"}}},FinishReason::tool_calls};
    ScriptStep answer; answer.last_role = Role::tool; answer.tool_names = {"change"}; answer.response = Response{{Role::assistant,"reported"}};
    ScriptedModel model({1,"goal_effect_guard",1,{},{generation,action,answer,generation,action,answer},6});
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    Session session({},channel,model.backend()); ConfirmationCaller caller; RunOptions options;
    const std::vector<std::string> ids{"fixture/change"}; options.tools = catalog.snapshot(ids,true); options.planning = PlanLimits{};
    options.now = model.clock(); options.confirmation_caller = caller; options.policy.confirm_changes = true;
    const auto request = session.begin("first goal",options);
    until(session,[&] { return request->confirmations().size() == 1; });
    auto pending = request->confirmations()[0]; session.decide(caller,{1,pending.id,pending.revision,pending.target,true});
    until(session,[&] { return request->plan()->steps[0].waiting == PlanWait::verification; });
    check(calls == 1 && request->tool_steps()[0].result == OperationState::failed && request->tool_steps()[0].effect == effect,"original effect was not retained");
    session.revise_goal(caller,request->plan()->definition->binding(),"explicitly revised goal");
    until(session,[&] { return request->confirmations().size() == 2; }); pending = request->confirmations()[1];
    check(pending.target.goal_revision == 2,"new approval still bound to old goal");
    const ConfirmationCommand command{2,pending.id,pending.revision,pending.target,true}; session.decide(caller,command);
    until(session,[&] { return request->plan()->steps[0].waiting == PlanWait::verification; });
    const auto tools = request->tool_steps();
    check(calls == 1 && tools.size() == 2 && tools[0].effect == effect && !tools[1].accepted && tools[1].effect == ToolEffect::not_applied,
        "new goal or approval replayed uncertain non-idempotent effect");
    session.decide(caller,command); request->request_cancel(CancelReason::user); until(session,[&] { return !session.busy(); }); model.verify_complete();
    check(calls == 1 && request->plan_history().size() == 1,"receipt retry or goal history lost effect guard");
}
}
// 固定模型和工具回执验证宿主效果治理，不执行真实模型裁判或外部动作。
int main() {
    try { repeated(ToolEffect::unknown); repeated(ToolEffect::applied); std::cout << "Goal effect guard checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
