#include "support/scripted_model.hpp"
#include <dafeiyu/plugin_tools.hpp>
#include <dafeiyu/session.hpp>
#include <dafeiyu/run_trace.hpp>
#include <charconv>
#include <iostream>
#include <thread>

namespace {
using namespace dfy::agent;
using namespace dfy::runtime;
using namespace dfy::model;
using namespace dfy::plugins;
using namespace dfy::tests;
// 报告原生连续行动、观察绑定或预算不一致。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 验证损坏关联不能冒充真实诊断。
void rejects(const auto& action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid observation association accepted");
}
// 有界推进原 Session，不把查询或刷新作为工作推进。
void until(Session& session, const auto& ready) {
    const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (!ready() && std::chrono::steady_clock::now() < end) { session.advance(); std::this_thread::yield(); }
    check(ready(),"native observation rebind stalled");
}
// 用单独只读授权实际采样，输入只来自原生回执而非模型描述。
// 步骤：提交只读工作并等待真实退出 → 解析原整数结果 → 构造宿主状态载荷。
std::shared_ptr<const StructValue> sample(const ToolSnapshot& scope, const std::string& name) {
    const RunId sampling{next_domain(),1}; ToolExecutor executor(sampling,scope);
    const auto request = executor.submit({{sampling,1},1},{"sample",name,"{}"});
    const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (request->exit()->state() == OperationState::pending && std::chrono::steady_clock::now() < end) { executor.advance(); std::this_thread::yield(); }
    const auto& result = request->outcome(); std::uint64_t value = 0;
    const auto parsed = std::from_chars(result.output.data(),result.output.data()+result.output.size(),value);
    check(request->exit()->state() == OperationState::succeeded && result.succeeded && result.effect == ToolEffect::not_applied &&
        parsed.ec == std::errc{} && parsed.ptr == result.output.data()+result.output.size(),"invalid sampling result");
    const FieldDescriptor fields[] = {{"value",ValueKind::uint64,0}};
    auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("fixture.rebinding",1,fields,{1,64}));
    const NamedValue values[] = {{"value",Value::from_uint64(value)}};
    return std::make_shared<const StructValue>(StructValue::create(type,values,64));
}
// 验证动作后完整重读、原运行换入新依据、下一动作及旧回执重试，全部保留原预算。
// 步骤：读取初值并规划两步 → 第一次动作及重采样 → 换入依据并验证原步骤 → 第二动作后重试旧回执 → 完成或在原限额停止。
void scenario(const Manifest& manifest, const std::filesystem::path& settings, bool limited) {
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(32,64),1,8);
    auto instance = std::make_shared<PluginInstance>(manifest,settings,channel); check(!instance->create() && !instance->start(),"native setup failed");
    auto catalog = std::make_shared<ToolCatalog>(); PluginTools registration(instance,catalog);
    const auto changes = catalog->snapshot(std::vector<std::string>{"test.tools/increment"},true);
    const auto reads = catalog->snapshot(std::vector<std::string>{"test.tools/read"});
    const auto name = changes.definitions()[0].name, read_name = reads.definitions()[0].name;
    dfy::platform::DynamicLibrary counters(manifest.entry);
    const auto count = reinterpret_cast<uint32_t (DFY_V2_CALL*)(uint32_t)>(counters.symbol("dfy_tool_test_count"));
    const auto starts = count(0), effects = count(1), releases = count(2);
    ScriptStep generation; generation.response = Response{{Role::assistant,R"({"schema_version":1,"steps":[{"id":"one","objective":"increment","completion_criterion":"value one","tools":["test.tools/increment"]},{"id":"two","objective":"increment again","completion_criterion":"value two","tools":["test.tools/increment"]}]})"}};
    ScriptStep action; action.tool_names = {name}; action.response = Response{{Role::assistant,"",{{"change",name,"{}"}}},FinishReason::tool_calls};
    ScriptStep answer; answer.tool_names = {name}; answer.last_role = Role::tool; answer.response = Response{{Role::assistant,"reported"}};
    auto extra = action; extra.last_role = Role::tool; extra.response->message.tool_calls[0].id = "extra";
    ScenarioSpec spec{1,"native_rebinding",1,{},{generation,action,answer,action,extra,answer},6};
    if (limited) { spec.steps.resize(4); spec.expected_model_requests = 4; }
    ScriptedModel model(std::move(spec)); ObservationSource source(instance->generation(),ObservationTrust::host_verified); ObservationInbox inbox({source.id()});
    const auto publish = [&](std::uint64_t revision) {
        const auto now = model.clock()(); inbox.accept(source.capture({"counter",1},revision,now,now+std::chrono::seconds(1),sample(reads,read_name)),now);
        return inbox.guard(inbox.latest(source.id(),"counter"),now);
    };
    const auto original = publish(1); Session session({},channel,model.backend()); ConfirmationCaller caller; RunOptions options;
    options.planning = PlanLimits{}; options.tools = changes; options.now = model.clock(); options.confirmation_caller = caller;
    options.policy.confirm_changes = true; if (limited) options.policy.model_limit = 4;
    options.action_observations.emplace(std::vector<ObservationGuard>{original}); const auto initial_binding = options.action_observations->binding();
    const auto request = session.begin("increment twice",options); until(session,[&] { return !request->confirmations().empty(); });
    const auto first = request->confirmations()[0]; const ConfirmationCommand first_command{1,first.id,first.revision,first.target,true};
    session.decide(caller,first_command); until(session,[&] { return request->plan()->steps[0].waiting == PlanWait::verification; });
    const auto key = request->plan()->steps[0].key;
    const auto refresh = session.request_observation_rebind(caller,key);
    const auto feedback = session.request_plan_feedback(caller,key,original,"value",Value::from_uint64(1));
    const auto observed = publish(2); const ActionObservations replacement({observed});
    const auto receipt = session.rebind_observations(caller,refresh,replacement);
    check(receipt.previous_binding == initial_binding && request->diagnostics().model_requests == 3 && count(1)-effects == 1,
        "rebinding reset counts or executed action inline");
    check(session.verify_plan_feedback(caller,feedback,observed).match == ObservationMatch::satisfied,"actual first state not verified");
    until(session,[&] { return request->confirmations().size() == 2; });
    const auto second = request->confirmations()[1];
    check(second.target.observation_binding == replacement.binding() && second.target.observation_binding != first.target.observation_binding,"new action kept old binding");
    session.decide(caller,first_command); check(count(1)-effects == 1,"old approval started new action");
    session.decide(caller,{2,second.id,second.revision,second.target,true}); until(session,[&] { return count(1)-effects == 2; });
    session.rebind_observations(caller,refresh,replacement);
    if (limited) until(session,[&] { return !session.busy(); });
    else {
        until(session,[&] { return request->plan()->steps[1].waiting == PlanWait::verification; });
        check(request->tool_steps().size() == 3 && !request->tool_steps()[2].accepted && request->tool_steps()[2].effect == ToolEffect::not_applied,
            "old rebind receipt rearmed consumed observations");
        const auto final_key = request->plan()->steps[1].key;
        const auto verification = session.request_plan_feedback(caller,final_key,observed,"value",Value::from_uint64(2));
        check(session.verify_plan_feedback(caller,verification,publish(3)).match == ObservationMatch::satisfied,"actual final state not verified");
        until(session,[&] { return !session.busy(); });
    }
    model.verify_complete(); const auto expected_calls = limited ? 4u : 5u;
    check(count(0)-starts == expected_calls && count(2)-releases == expected_calls && count(1)-effects == 2 &&
        request->observation_rebinds().size() == 1 && request->operation()->state() == (limited ? OperationState::failed : OperationState::succeeded),
        "native continuation lost effects, budget or release");
    if (limited) check(request->diagnostics().stop_reason == PolicyReason::model_limit,"rebinding renewed model budget");
    const auto trace = run_timeline(*request,"native-rebind"); check(review_timeline(trace).run_exited && trace.observation_rebinds.size() == 1 &&
        trace.model_contexts[2].observation_binding == initial_binding && trace.model_contexts[2].observations_current == false &&
        trace.model_contexts[3].observation_binding == replacement.binding() && trace.model_contexts[3].observations_current == true,
        "observation refresh rewrote old model context");
    check(trace.versions.tools[0].plugin == std::string(instance->plugin_version()) && trace.versions.tools[0].schema->starts_with("sha256:"),"native public versions missing");
    for (const auto& node : trace.nodes) if (node.id.kind == TraceNodeKind::tool && node.accepted == true)
        check(node.tool_id == "test.tools/increment","tool trace did not identify original capability");
    auto damaged = trace; damaged.model_contexts[0].observation_binding = replacement.binding(); rejects([&] { review_timeline(damaged); });
    damaged = trace; damaged.observation_rebinds[0].after_model_requests = 2; rejects([&] { review_timeline(damaged); });
    damaged = trace; damaged.observation_rebinds[0].observations[0].revision = 1; rejects([&] { review_timeline(damaged); });
    check(instance->shutdown() == ShutdownResult::closed,"rebind values pinned native code");
}
}
// 真实采样与动作使用原测试 DLL，规划和模型响应全部固定。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 3,"expected package and settings"); const auto manifest = load_manifest(argv[1]);
        scenario(manifest,argv[2],false); scenario(manifest,argv[2],true); std::cout << "Native observation rebind checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
