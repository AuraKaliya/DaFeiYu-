#include "../modules/agent/tests/support/compiled_replay.hpp"
#include <dafeiyu/observation_controller.hpp>
#include <dafeiyu/plugin_tools.hpp>
#include <dafeiyu/run_trace.hpp>
#include <dafeiyu/diagnostic_snapshot.hpp>
#include <dafeiyu/diagnostic_json.hpp>
#include <charconv>
#include <iostream>
#include <thread>

namespace {
using namespace dfy::agent;
using namespace dfy::runtime;
using namespace dfy::model;
using namespace dfy::plugins;
using namespace dfy::tests;
constexpr std::string_view cases[]{"native_action_satisfied","native_action_mismatch","native_action_unknown"};
constexpr std::string_view observed_cases[]{"native_observation_action","native_observation_revoked"};
// 报告真实读取、动作和后续反馈的因果或效果错误。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 验证尚未实际退出或不属于原步骤的工具不能作为采样起点。
void rejects(const auto& action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid native feedback accepted");
}
// 有界推进原会话，等待真实资源退出或明确验证状态。
void until(Session& session, const auto& ready) {
    const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (!ready() && std::chrono::steady_clock::now() < end) { session.advance(); std::this_thread::yield(); }
    check(ready(),"native feedback run stalled");
}
// 宿主测试适配器仅解析真实只读工具的完整整数回执，拒绝模型文字和失败结果。
std::shared_ptr<const StructValue> observed(const ToolResult& result) {
    std::uint64_t value = 0; const auto parsed = std::from_chars(result.output.data(),result.output.data()+result.output.size(),value);
    check(result.succeeded && result.effect == ToolEffect::not_applied && parsed.ec == std::errc{} && parsed.ptr == result.output.data()+result.output.size(),"invalid native state receipt");
    const FieldDescriptor fields[] = {{"value",ValueKind::uint64,0}};
    auto schema = std::make_shared<const StructDescriptor>(StructDescriptor::create("fixture.feedback",1,fields,{1,64}));
    const NamedValue values[] = {{"value",Value::from_uint64(value)}};
    return std::make_shared<const StructValue>(StructValue::create(schema,values,64));
}
// 先由独立且只读的宿主采样工作取得初值，尚未创建用户运行或动作授权。
ToolResult initial_read(const ToolSnapshot& tools, const std::string& name) {
    const RunId sampling{next_domain(),1}; ToolExecutor executor(sampling,tools);
    const auto request = executor.submit({{sampling,1},1},{"initial",name,"{}"}); const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (request->exit()->state() == OperationState::pending && std::chrono::steady_clock::now() < end) { executor.advance(); std::this_thread::yield(); }
    check(request->exit()->state() == OperationState::succeeded,"initial native sample did not exit"); return request->outcome();
}
// 验证真实读状态→原授权变更→同运行只读重查→新观察反馈，模型完成文字不能替代证据。
// 步骤：读取初值并固定前置 → 执行获准动作 → 原动作退出后签发反馈 → 读取真实后值 → 按满足、不满足或未知分流。
void scenario(const Manifest& manifest, const std::filesystem::path& settings, std::size_t mode, std::ostream* evidence = nullptr) {
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(16,64),1,8);
    auto instance = std::make_shared<PluginInstance>(manifest,settings,channel); check(!instance->create() && !instance->start(),"native setup failed");
    auto catalog = std::make_shared<ToolCatalog>(); PluginTools registration(instance,catalog);
    const std::vector<std::string> ids{"test.tools/increment","test.tools/read"}; const auto scope = catalog->snapshot(ids,true);
    const auto read_scope = scope.restrict_to(std::vector<std::string>{ids[1]}); const auto read_name = read_scope.definitions()[0].name;
    const auto change_name = scope.restrict_to(std::vector<std::string>{ids[0]}).definitions()[0].name;
    dfy::platform::DynamicLibrary counter(manifest.entry);
    const auto count = reinterpret_cast<uint32_t (DFY_V2_CALL*)(uint32_t)>(counter.symbol("dfy_tool_test_count"));
    const auto starts = count(0), effects = count(1), releases = count(2);
    ObservationSource source(instance->generation(),ObservationTrust::host_verified); ObservationInbox inbox({source.id()});
    inbox.accept(source.capture({"counter",1},1,{},ObservationTime{}+std::chrono::seconds(1),observed(initial_read(read_scope,read_name))),{});
    const auto baseline = inbox.guard(inbox.latest(source.id(),"counter"),{});
    check(ObservationExpectation(baseline,"value",Value::from_uint64(1)).assess(baseline,{}).match == ObservationMatch::not_satisfied,"initial state already claimed target");
    ScriptStep generation; generation.response = Response{{Role::assistant,R"({"schema_version":1,"steps":[{"id":"change","objective":"increment and read","completion_criterion":"host checks actual value","tools":["test.tools/increment","test.tools/read"]}]})"}};
    ScriptStep action; action.tool_names = {change_name,read_name}; action.response = Response{{Role::assistant,"",{{"change",change_name,"{}"}}},FinishReason::tool_calls};
    ScriptStep read; read.last_role = Role::tool; read.last_tool_id = "change"; read.tool_names = {change_name,read_name};
    read.response = Response{{Role::assistant,"",{{"read",read_name,"{}"}}},FinishReason::tool_calls};
    ScriptStep final; final.last_role = Role::tool; final.last_tool_id = "read"; final.tool_names = {change_name,read_name}; final.response = Response{{Role::assistant,"model claims target reached"}};
    ScriptedModel model({1,std::string(cases[mode]),1,{},{generation,action,read,final},4});
    Session session(fixed_replay_model(),channel,model.backend()); ConfirmationCaller caller;
    RunOptions options; options.tools = scope; options.now = model.clock(); options.planning = PlanLimits{}; options.confirmation_caller = caller;
    options.version_labels = compiled_replay_labels("agent.plan-request-v1");
    options.policy.confirm_changes = true; options.action_observations.emplace(std::vector<ObservationGuard>{baseline});
    const auto request = session.begin("increment once",options); until(session,[&] { return !request->confirmations().empty(); });
    const auto pending = request->confirmations()[0]; session.decide(caller,{1,pending.id,pending.revision,pending.target,true});
    until(session,[&] { return count(1) > effects; }); const auto key = request->plan()->steps[0].key; const auto candidate = request->tool_steps()[0].candidate;
    rejects([&] { session.request_plan_feedback(caller,key,baseline,"value",Value::from_uint64(1),candidate); });
    until(session,[&] { return request->tool_steps()[0].exit == OperationState::succeeded; });
    rejects([&] { session.request_plan_feedback(caller,key,baseline,"value",Value::from_uint64(1),candidate+1000); });
    const auto ticket = session.request_plan_feedback(caller,key,baseline,"value",Value::from_uint64(mode == 1 ? 2 : 1),candidate);
    check(ticket.completed_tool() == candidate && request->plan()->steps[0].waiting == PlanWait::work,"feedback was not issued between action and read");
    until(session,[&] { return request->plan()->steps[0].waiting == PlanWait::verification; });
    const auto read_result = request->tool_result(request->tool_steps()[1].candidate); check(read_result.has_value(),"actual read result unavailable");
    const auto now = model.clock()(); inbox.accept(source.capture({"counter",1},2,now,now+std::chrono::seconds(1),observed(*read_result)),now);
    const auto current = inbox.guard(inbox.latest(source.id(),"counter"),now); if (mode == 2) source.revoke();
    const auto assessment = session.verify_plan_feedback(caller,ticket,current);
    const auto diagnostic = diagnostic_snapshot(*request,"feedback-summary");
    check(diagnostic.feedback.size() == 1 && diagnostic.feedback[0].request == ticket.id() && diagnostic.feedback[0].checks == 1 &&
        diagnostic.feedback[0].latest->match == assessment.match && diagnostic.feedback[0].verified == (mode == 0) && diagnostic.plans.size() == 1,
        "diagnostic lost feedback or copied false completion");
    const auto encoded = encode_diagnostic(diagnostic); const auto decoded = decode_diagnostic(encoded);
    check(decoded.feedback[0].latest->match == assessment.match && encode_diagnostic(decoded) == encoded,"native feedback diagnostic did not round-trip");
    check(assessment.match == (mode == 0 ? ObservationMatch::satisfied : mode == 1 ? ObservationMatch::not_satisfied : ObservationMatch::unknown),"native feedback confused target and evidence");
    if (mode) { check(request->operation()->state() == OperationState::pending,"model text completed an unverified target"); request->request_cancel(CancelReason::user); }
    until(session,[&] { return !session.busy(); }); model.verify_complete();
    check(count(0)-starts == 3 && count(1)-effects == 1 && count(2)-releases == 3 && request->tool_steps()[0].effect == ToolEffect::applied &&
        review_timeline(run_timeline(*request,"native-feedback")).run_exited,"feedback repeated action or lost actual exit");
    check(instance->shutdown() == ShutdownResult::closed,"feedback snapshots pinned native code");
    if (evidence) write_compiled_replay(*evidence,model.report(),*request,count(0)-starts-1);
}

// 以原生初读作为观察条件，验证审批后的实际动作及在途撤销仍保留效果。
// 步骤：绑定实际初值和单工具 → 完成审批 → 检查退场前保持 DLL → 在成功或撤销后读回实际值。
void observed_action(const Manifest& manifest, const std::filesystem::path& settings, bool revoke, std::ostream* evidence = nullptr) {
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(16,64),1,8);
    auto instance = std::make_shared<PluginInstance>(manifest,settings,channel);
    check(!instance->create() && !instance->start(),"native observation action setup failed");
    auto catalog = std::make_shared<ToolCatalog>(); PluginTools registration(instance,catalog);
    const std::vector<std::string> change_ids{"test.tools/increment"}, read_ids{"test.tools/read"};
    const auto action = catalog->snapshot(change_ids,true), read = catalog->snapshot(read_ids);
    const auto action_name = action.definitions()[0].name, read_name = read.definitions()[0].name;
    const auto initial = initial_read(read,read_name);
    check(initial.output == "0","native initial state was not reset");

    ScriptStep candidate; candidate.tool_names = {action_name};
    candidate.response = Response{{Role::assistant,"",{{"change",action_name,"{}"}}},FinishReason::tool_calls};
    ScriptStep final; final.last_role = Role::tool; final.last_tool_id = "change"; final.tool_names = {action_name};
    final.response = Response{{Role::assistant,"increment receipt recorded"}};
    ScenarioSpec spec{1,std::string(observed_cases[revoke ? 1 : 0]),1,{},{candidate},1};
    if (!revoke) { spec.steps.push_back(final); spec.expected_model_requests = 2; }
    ScriptedModel model(std::move(spec)); Session session(fixed_replay_model(),channel,model.backend());
    ConfirmationCaller caller;
    ObservationSource source(instance->generation(),ObservationTrust::host_verified); ObservationInbox inbox({source.id()});
    const auto state = observed(initial);
    inbox.accept(source.capture({"counter",1},1,{},ObservationTime{}+std::chrono::seconds(1),state),{});
    RunOptions options; options.now = model.clock(); options.tools = action;
    options.policy.allow_observation = options.policy.confirm_changes = true;
    options.confirmation_caller = caller; options.version_labels = compiled_replay_labels("none");
    ObservationController controller(session,inbox,{source.id(),{"counter",1},2,1,2,std::chrono::milliseconds(1),ObservationTime{}+std::chrono::seconds(1)},
        "increment on verified zero",options,ObservationAction{change_ids[0],"value",Value::from_uint64(0),state->descriptor()});
    controller.enable(); const auto request = controller.poll().request;
    check(static_cast<bool>(request),"native observation did not admit a task");
    until(session,[&] { return !request->confirmations().empty(); });
    const auto pending = request->confirmations()[0];
    check(!request->tool_steps()[0].accepted && pending.target.observation_binding == request->initial_observation_binding(),
        "native change admitted before confirmation");
    session.decide(caller,{1,pending.id,pending.revision,pending.target,true});
    until(session,[&] { return request->tool_steps()[0].result == OperationState::succeeded; });
    check(request->tool_steps()[0].exit == OperationState::pending && instance->shutdown() == ShutdownResult::in_use,
        "native change result released code before actual exit");
    if (revoke) {
        source.revoke();
        check(controller.poll().decision == TriggerDecision::stale,"revoked observation did not request original stop");
    }
    until(session,[&] { return !session.busy(); }); model.verify_complete();
    const auto trace = run_timeline(*request,"native-observation-action");
    const auto facts = review_timeline(trace);
    check(facts.tools == 1 && facts.applied_effects == 1 && facts.run_exited && initial_read(read,read_name).output == "1" &&
        request->operation()->state() == (revoke ? OperationState::cancelled : OperationState::succeeded),
        "native readback or original exit lost the applied effect");
    check(controller.poll().decision == (revoke ? TriggerDecision::stopped : TriggerDecision::duplicate) &&
        instance->shutdown() == ShutdownResult::closed,"native rule restarted or retained released DLL");
    if (evidence) write_compiled_replay(*evidence,model.report(),*request,1);
}
}
// 以固定模型和指定测试包验证动作及反馈，或采集一个明确命名的案例。
// 步骤：核对夹具及参数 → 运行指定案例或全部反馈场景 → 检查原生退出并输出。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 3 || (argc == 5 && std::wstring_view(argv[3]) == L"--capture"),"expected package, settings and optional --capture CASE");
        const auto manifest = load_manifest(argv[1]);
        check(manifest.id == "test.tools","unexpected native feedback fixture");
        if (argc == 5) {
            const auto id = std::filesystem::path(argv[4]).string();
            const auto found = std::find(std::begin(cases),std::end(cases),id);
            if (found != std::end(cases)) scenario(manifest,argv[2],static_cast<std::size_t>(found-std::begin(cases)),&std::cout);
            else {
                check(id == observed_cases[0] || id == observed_cases[1],"unknown native feedback replay case");
                observed_action(manifest,argv[2],id == observed_cases[1],&std::cout);
            }
        } else {
            for (std::size_t mode = 0; mode < std::size(cases); ++mode) scenario(manifest,argv[2],mode);
            observed_action(manifest,argv[2],false);
            observed_action(manifest,argv[2],true);
            std::cout << "Native action feedback checks passed\n";
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
