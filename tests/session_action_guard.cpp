#include "support/scripted_model.hpp"
#include <dafeiyu/session.hpp>
#include <iostream>
#include <thread>

namespace {
using namespace dfy::agent;
using namespace dfy::runtime;
using namespace dfy::model;
using namespace dfy::tests;
enum class Change { none, waiting, approved, admitted, callback, expired, destroyed, revoked };
// 报告原观察、确认和实际动作边界不一致。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 验证不完整或已失效的前置条件不能当作动作许可。
void rejects(const auto& action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid action precondition accepted");
}
// 生成无设备的宿主状态，载荷不参与控制身份签发。
std::shared_ptr<const StructValue> payload() {
    auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("fixture.guard",1,{}, {1,64}));
    return std::make_shared<const StructValue>(StructValue::create(type,{},64));
}
// 有界推进正式会话，查询不执行模型或工具。
void until(Session& session, const auto& ready) {
    const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (!ready() && std::chrono::steady_clock::now() < end) { session.advance(); std::this_thread::yield(); }
    check(ready(),"action guard scenario stalled");
}
// 验证等待、批准和实际接纳之间的观察变化均不能绕过最终动作检查。
// 步骤：固定原观察与候选 → 等待人工确认 → 在指定边界改变状态 → 核对零动作或唯一原动作。
void scenario(Change change) {
    ObservationSource source(1,ObservationTrust::host_verified); auto inbox = std::make_unique<ObservationInbox>(std::vector<DomainId>{source.id()});
    const auto data = payload();
    const auto sample = [&](std::uint64_t revision) { return source.capture({"object",1},revision,{},ObservationTime{}+std::chrono::milliseconds(10),data); };
    inbox->accept(sample(1),{}); const auto observed = inbox->latest(source.id(),"object"); const auto guard = inbox->guard(observed,{});
    rejects([&] { ActionObservations invalid({guard,guard}); });
    ToolCatalog catalog; unsigned calls = 0;
    catalog.add({"fixture/action",1,1,{"action","fixture",R"({"type":"object","properties":{},"additionalProperties":false})"},true,false},
        [&](const auto&) { ++calls; return completed_tool({true,ToolEffect::applied,"changed",ToolError::none}); });
    ScriptStep action; action.tool_names = {"action"}; action.response = Response{{Role::assistant,"",{{"call","action","{}"}}},FinishReason::tool_calls};
    ScriptStep final; final.tool_names = {"action"}; final.last_role = Role::tool; final.response = Response{{Role::assistant,"reported"}};
    ScriptedModel model({1,"action_guard",1,{},{action,final},2});
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    Session session({},channel,model.backend()); ConfirmationCaller caller; RunOptions options;
    const std::vector<std::string> ids{"fixture/action"}; options.tools = catalog.snapshot(ids,true); options.now = model.clock();
    options.action_observations.emplace(std::vector<ObservationGuard>{guard}); options.confirmation_caller = caller; options.policy.confirm_changes = true;
    std::shared_ptr<TurnRequest> request;
    options.extension = [&](ControlPoint point,const PolicyContext&) {
        if (change == Change::callback && point == ControlPoint::tool_start && request && !request->confirmations().empty() &&
            request->confirmations()[0].state == ConfirmationState::approved) inbox->accept(sample(2),model.clock()());
        return PolicyDecision{PolicyVerdict::allow,PolicyReason::none};
    };
    request = session.begin("host goal",options); until(session,[&] { return !request->confirmations().empty(); });
    const auto pending = request->confirmations()[0]; const ConfirmationCommand command{1,pending.id,pending.revision,pending.target,true};
    check(pending.target.observation_binding == options.action_observations->binding() && !calls && request->observation_inputs().size() == 1,
        "confirmation or model input used different observations");
    if (change == Change::waiting) inbox->accept(sample(2),{});
    if (change == Change::expired) model.advance_clock(std::chrono::milliseconds(10));
    if (change == Change::destroyed) inbox.reset();
    if (change == Change::revoked) source.revoke();
    if (change == Change::waiting || change == Change::expired || change == Change::destroyed || change == Change::revoked)
        rejects([&] { session.decide(caller,command); });
    else {
        const auto receipt = session.decide(caller,command);
        if (change == Change::admitted) { session.advance(); check(request->tool_steps()[0].accepted && !calls,"admission executed tool inline"); }
        if (change == Change::approved || change == Change::admitted) inbox->accept(sample(2),{});
        check(session.decide(caller,command) == receipt && !calls,"receipt retry revalidated as new permission");
    }
    until(session,[&] { return !session.busy(); }); model.verify_complete(); const auto step = request->tool_steps()[0];
    check(calls == (change == Change::none ? 1u : 0u) && step.effect == (change == Change::none ? ToolEffect::applied : ToolEffect::not_applied),
        "stale observed action executed or lost original effect");
    if (change == Change::admitted || change == Change::callback) check(step.accepted && step.result == OperationState::failed,"last check lost accepted-but-unstarted fact");
}
// 验证仅有历史采样而无当前性绑定的变更请求在模型提交前拒绝。
void missing_guard() {
    ObservationSource source(1,ObservationTrust::host_verified); ObservationInbox inbox({source.id()});
    inbox.accept(source.capture({"object",1},1,{},ObservationTime{}+std::chrono::seconds(1),payload()),{});
    ToolCatalog catalog; catalog.add({"fixture/action",1,1,{"action","fixture",R"({"type":"object","properties":{},"additionalProperties":false})"},true,false});
    const std::vector<std::string> ids{"fixture/action"}; ScriptedModel model({1,"missing_guard",1,{}, {},0});
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    Session session({},channel,model.backend()); RunOptions options; options.tools = catalog.snapshot(ids,true); options.now = model.clock();
    options.observations = {inbox.latest(source.id(),"object")}; rejects([&] { session.begin("host goal",options); }); model.verify_complete();
}
}
// 使用合成观察和固定响应验证确定性动作控制，不调用真实模型。
int main() {
    try {
        for (const auto change : {Change::none,Change::waiting,Change::approved,Change::admitted,Change::callback,Change::expired,Change::destroyed,Change::revoked}) scenario(change);
        missing_guard(); std::cout << "Session action guard checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
