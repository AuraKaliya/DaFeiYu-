#include "support/compiled_replay.hpp"
#include <dafeiyu/observation_controller.hpp>
#include <iostream>
#include <thread>

namespace {
using namespace dfy::agent;
using namespace dfy::runtime;
using namespace dfy::model;
using namespace dfy::tests;
enum class Outcome { applied, denied, changed_basis, expired, partial_failure, unknown_effect, omitted };
constexpr std::string_view cases[]{"observation_action_applied","observation_action_denied","observation_action_changed_basis",
    "observation_action_expired","observation_action_partial","observation_action_unknown","observation_action_omitted"};

// 报告自动接纳、实际动作或停止不符合宿主规则。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

// 在虚拟业务时钟之外用真实看门狗限制测试推进。
void until(Session& session, const auto& ready) {
    const auto timeout = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (!ready() && std::chrono::steady_clock::now() < timeout) { session.advance(); std::this_thread::yield(); }
    check(ready(),"observation action did not reach expected boundary");
}

// 创建同一结构类型中的明确布尔条件。
std::shared_ptr<const StructValue> sample_value(bool ready, std::uint32_t version = 1) {
    const FieldDescriptor fields[] = {{"ready",ValueKind::boolean,0}};
    auto schema = std::make_shared<const StructDescriptor>(StructDescriptor::create("fixture.action_state",version,fields,{1,64}));
    const NamedValue values[] = {{"ready",Value::from_bool(ready)}};
    return std::make_shared<const StructValue>(StructValue::create(schema,values,64));
}

// 验证绑定条件的观察动作及零效果或部分效果后的永久停止。
// 步骤：过滤未满足采样 → 固定原候选及审批 → 在指定边界推进或改变依据 → 核对真实退出和后续触发。
void verify(Outcome outcome, std::ostream* evidence = nullptr) {
    ScriptStep action; action.tool_names = {"apply"};
    action.response = outcome == Outcome::omitted ? Response{{Role::assistant,"I already did it"}} :
        Response{{Role::assistant,"",{{"one","apply","{}"}}},FinishReason::tool_calls};
    ScriptStep finish; finish.last_role = Role::tool; finish.last_tool_id = "one"; finish.tool_names = {"apply"};
    finish.response = Response{{Role::assistant,"recorded actual tool result"}};
    ScenarioSpec spec{1,std::string(cases[static_cast<std::size_t>(outcome)]),1,{},{action},1};
    if (outcome == Outcome::applied) { spec.steps.push_back(finish); spec.expected_model_requests = 2; }
    ScriptedModel model(std::move(spec)); const auto clock = model.clock();
    auto events = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    Session session(fixed_replay_model(),events,model.backend()); ConfirmationCaller caller;
    ObservationSource source(1,ObservationTrust::host_verified); ObservationInbox inbox({source.id()});
    // 接纳固定对象的新采样，虚拟时钟保持可精确复现。
    const auto sample = [&](std::uint64_t revision, bool ready, std::uint32_t version = 1) {
        const auto now = clock();
        return inbox.accept(source.capture({"object",1},revision,now,now+std::chrono::milliseconds(40),sample_value(ready,version)),now);
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
    options.version_labels = compiled_replay_labels("none");
    ObservationController controller(session,inbox,{source.id(),{"object",1},7,1,2,std::chrono::milliseconds(1),ToolTime{}+std::chrono::seconds(1)},
        "apply one eligible change",options,ObservationAction{tool,"ready",Value::from_bool(true),sample_value(true)->descriptor()});
    controller.enable(); sample(1,true,2);
    check(controller.poll().decision == TriggerDecision::condition_unknown && !controller.runs(),"changed schema started an action");
    sample(2,false);
    check(controller.poll().decision == TriggerDecision::condition_not_met && !controller.runs() && !model.report().attempted,"false condition submitted a run");
    sample(3,true);
    check(controller.poll(true).decision == TriggerDecision::user_pending,"observation displaced pending user input");
    auto admitted = controller.poll(); auto request = std::move(admitted.request);
    check(request && admitted.receipt && admitted.receipt->run == request->run_id() && admitted.receipt->ticket.observation_revision == 3 &&
        request->initial_observation_binding() && request->diagnostics().source == RunOrigin::observation_rule,"condition lost its original binding");
    if (outcome != Outcome::omitted) {
        until(session,[&] { return !request->confirmations().empty(); });
        const auto pending = request->confirmations()[0];
        check(!calls && pending.target.observation_binding == request->initial_observation_binding(),"waiting action used different evidence");
        const ConfirmationCommand command{1,pending.id,pending.revision,pending.target,outcome != Outcome::denied};
        if (outcome == Outcome::expired) {
            model.advance_clock(std::chrono::milliseconds(40));
            bool rejected = false;
            try { session.decide(caller,command); } catch (const std::invalid_argument&) { rejected = true; }
            check(rejected,"expired sample accepted original approval");
        } else {
            const auto receipt = session.decide(caller,command);
            check(session.decide(caller,command) == receipt && !calls,"decision retry executed the original action");
        }
        if (outcome == Outcome::changed_basis) {
            session.advance();
            check(request->tool_steps()[0].accepted && !calls,"action was not deferred until final before-start check");
            sample(4,true);
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
        const auto stopped_run = request->run_id();
        request.reset();
        model.advance_clock(std::chrono::milliseconds(1)); sample(5,true);
        const auto stopped = controller.poll();
        check(stopped.decision == TriggerDecision::stopped && stopped.request && stopped.request->run_id() == stopped_run &&
            controller.runs() == 1 && !session.closed(),
            "failed action left trigger available or stopped the shared session");
        request = stopped.request;
    }
    if (evidence) write_compiled_replay(*evidence,model.report(),*request,calls);
}

// 验证缺少原数据契约、配置字段错误或不可信来源均无法合成写入许可。
// 步骤：建立零调用替身 → 校验不合法的宿主配置 → 送入不可信的匹配值 → 核对零接纳。
void untrusted_condition() {
    ScriptedModel model({1,"untrusted_action_condition",1,{}, {},0});
    auto events = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    Session session({},events,model.backend());
    ObservationSource source(1,ObservationTrust::external_data); ObservationInbox inbox({source.id()});
    const auto sample = sample_value(true);
    inbox.accept(source.capture({"object",1},1,{},ObservationTime{}+std::chrono::seconds(1),sample),{});
    ToolCatalog tools;
    const std::string tool = "fixture/apply";
    tools.add({tool,1,1,{"apply","fixture",R"({"type":"object","properties":{},"additionalProperties":false})"},true,false},
        [](const auto&) -> std::unique_ptr<ToolWork> { throw std::runtime_error("untrusted source invoked tool"); });
    RunOptions options; options.now = model.clock(); options.tools = tools.snapshot(std::span(&tool,1),true);
    options.policy.allow_observation = options.policy.confirm_changes = true; options.confirmation_caller.emplace();
    const ObservationRule rule{source.id(),{"object",1},1,1,1,std::chrono::milliseconds(1),ObservationTime{}+std::chrono::seconds(1)};
    unsigned rejected = 0;
    for (const auto& invalid : {ObservationAction{tool,"ready",Value::from_bool(true),{}},
        ObservationAction{tool,"other",Value::from_bool(true),sample->descriptor()}}) {
        try { ObservationController controller(session,inbox,rule,"goal",options,invalid); }
        catch (const std::invalid_argument&) { ++rejected; }
    }
    ObservationController controller(session,inbox,rule,"goal",options,ObservationAction{tool,"ready",Value::from_bool(true),sample->descriptor()});
    controller.enable();
    check(rejected == 2 && controller.poll().decision == TriggerDecision::condition_unknown && !controller.runs() && !session.busy(),
        "untrusted state or incorrect schema granted mutation");
    model.verify_complete();
}
}

// 只使用固定模型和内存工具检查明确的观察变更入口。
// 步骤：解析单案例采集或执行原验证矩阵 → 核对效果、退出和规则停止 → 报告结果。
int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--capture") {
            const auto found = std::find(std::begin(cases),std::end(cases),argv[2]);
            check(found != std::end(cases),"unknown observation action case");
            verify(static_cast<Outcome>(found-std::begin(cases)),&std::cout);
        } else {
            check(argc == 1,"usage: --capture CASE");
            for (std::size_t mode = 0; mode < std::size(cases); ++mode) verify(static_cast<Outcome>(mode));
            untrusted_condition();
            std::cout << "Observation action checks passed\n";
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
