#include "support/scripted_model.hpp"
#include <dafeiyu/observation_controller.hpp>
#include <iostream>
#include <thread>

namespace {
using namespace dfy::agent;
using namespace dfy::runtime;
using namespace dfy::model;
using namespace dfy::tests;
enum class Outcome { applied, denied, changed_basis, partial_failure, unknown_effect, omitted };

// 报告自动接纳、实际动作或停止不符合宿主规则。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

// 在虚拟业务时钟之外用真实看门狗限制测试推进。
void until(Session& session, const auto& ready) {
    const auto timeout = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (!ready() && std::chrono::steady_clock::now() < timeout) { session.advance(); std::this_thread::yield(); }
    check(ready(),"observation action did not reach expected boundary");
}

// 创建同一结构类型中的明确布尔条件。
std::shared_ptr<const StructValue> sample_value(bool ready) {
    const FieldDescriptor fields[] = {{"ready",ValueKind::boolean,0}};
    auto schema = std::make_shared<const StructDescriptor>(StructDescriptor::create("fixture.action_state",1,fields,{1,64}));
    const NamedValue values[] = {{"ready",Value::from_bool(ready)}};
    return std::make_shared<const StructValue>(StructValue::create(schema,values,64));
}

// 验证绑定条件的观察动作及零效果或部分效果后的永久停止。
// 步骤：过滤未满足采样 → 固定原候选及审批 → 在指定边界推进或改变依据 → 核对真实退出和后续触发。
void verify(Outcome outcome) {
    ScriptStep action; action.tool_names = {"apply"};
    action.response = outcome == Outcome::omitted ? Response{{Role::assistant,"I already did it"}} :
        Response{{Role::assistant,"",{{"one","apply","{}"}}},FinishReason::tool_calls};
    ScriptStep finish; finish.last_role = Role::tool; finish.last_tool_id = "one"; finish.tool_names = {"apply"};
    finish.response = Response{{Role::assistant,"recorded actual tool result"}};
    ScenarioSpec spec{1,"observation_action",1,{},{action},1};
    if (outcome == Outcome::applied) { spec.steps.push_back(finish); spec.expected_model_requests = 2; }
    ScriptedModel model(std::move(spec)); const auto clock = model.clock();
    auto events = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    Session session({},events,model.backend()); ConfirmationCaller caller;
    ObservationSource source(1,ObservationTrust::host_verified); ObservationInbox inbox({source.id()});
    // 接纳固定对象的新采样，虚拟时钟保持可精确复现。
    const auto sample = [&](std::uint64_t revision, bool ready) {
        const auto now = clock();
        return inbox.accept(source.capture({"object",1},revision,now,now+std::chrono::milliseconds(40),sample_value(ready)),now);
    };
    unsigned calls = 0; ToolCatalog tools;
    // 当前结果由固定夹具给出，记录工厂是否实际执行。
    const auto registration = tools.add({"fixture/apply",1,1,{"apply","fixture",R"({"type":"object","properties":{},"additionalProperties":false})"},true,false},
        [&](const auto&) {
            ++calls;
            if (outcome == Outcome::unknown_effect) return completed_tool({false,ToolEffect::unknown,"",ToolError::failed});
            if (outcome == Outcome::partial_failure) return completed_tool({false,ToolEffect::applied,"",ToolError::failed});
            return completed_tool({true,ToolEffect::applied,"recorded",ToolError::none});
        });
    check(registration != 0,"fixture registration missing");
    const std::string tool = "fixture/apply";
    RunOptions options; options.now = clock; options.tools = tools.snapshot(std::span(&tool,1),true);
    options.policy.allow_observation = options.policy.confirm_changes = true; options.confirmation_caller = caller;
    ObservationController controller(session,inbox,{source.id(),{"object",1},7,1,2,std::chrono::milliseconds(1),ToolTime{}+std::chrono::seconds(1)},
        "apply one eligible change",options,ObservationAction{tool,"ready",Value::from_bool(true)});
    controller.enable(); sample(1,false);
    check(controller.poll().decision == TriggerDecision::condition_not_met && !controller.runs() && !model.report().attempted,"false condition submitted a run");
    sample(2,true);
    check(controller.poll(true).decision == TriggerDecision::user_pending,"observation displaced pending user input");
    const auto admitted = controller.poll(); const auto request = admitted.request;
    check(request && admitted.receipt && admitted.receipt->run == request->run_id() && admitted.receipt->ticket.observation_revision == 2 &&
        request->initial_observation_binding() && request->diagnostics().source == RunOrigin::observation_rule,"condition lost its original binding");
    if (outcome != Outcome::omitted) {
        until(session,[&] { return !request->confirmations().empty(); });
        const auto pending = request->confirmations()[0];
        check(!calls && pending.target.observation_binding == request->initial_observation_binding(),"waiting action used different evidence");
        session.decide(caller,{1,pending.id,pending.revision,pending.target,outcome != Outcome::denied});
        if (outcome == Outcome::changed_basis) {
            session.advance();
            check(request->tool_steps()[0].accepted && !calls,"action was not deferred until final before-start check");
            sample(3,true);
        }
    }
    until(session,[&] { return !session.busy(); }); model.verify_complete();
    const bool applied = outcome == Outcome::applied;
    const auto effect = outcome == Outcome::unknown_effect ? ToolEffect::unknown : applied || outcome == Outcome::partial_failure ?
        ToolEffect::applied : ToolEffect::not_applied;
    const bool attempted = applied || outcome == Outcome::partial_failure || outcome == Outcome::unknown_effect;
    const auto steps = request->tool_steps();
    check(calls == static_cast<unsigned>(attempted) && request->exit()->state() == OperationState::succeeded &&
        request->operation()->state() == (applied ? OperationState::succeeded : OperationState::failed) &&
        request->diagnostics().stop_reason == (applied ? PolicyReason::none : PolicyReason::invalid_result),
        "action did not report its actual execution and stop");
    check(outcome == Outcome::omitted ? steps.empty() : steps.size() == 1 && steps[0].effect == effect,
        "observed action lost settled effect");
    if (applied) {
        check(controller.poll().decision == TriggerDecision::duplicate,"completed sample was replayed");
    } else {
        model.advance_clock(std::chrono::milliseconds(1)); sample(4,true);
        const auto stopped = controller.poll();
        check(stopped.decision == TriggerDecision::stopped && stopped.request == request && controller.runs() == 1 && !session.closed(),
            "failed action left trigger available or stopped the shared session");
    }
}
}

// 只使用固定模型和内存工具检查明确的观察变更入口。
// 步骤：执行成功及五种停止场景 → 核对真实效果和退出 → 报告验证结果。
int main() {
    try {
        for (const auto outcome : {Outcome::applied,Outcome::denied,Outcome::changed_basis,Outcome::partial_failure,Outcome::unknown_effect,Outcome::omitted})
            verify(outcome);
        std::cout << "Observation action checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
