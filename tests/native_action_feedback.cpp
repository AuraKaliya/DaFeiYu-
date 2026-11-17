#include "support/scripted_model.hpp"
#include <dafeiyu/plugin_tools.hpp>
#include <dafeiyu/run_trace.hpp>
#include <dafeiyu/diagnostic_snapshot.hpp>
#include <charconv>
#include <iostream>
#include <thread>

namespace {
using namespace dfy::agent;
using namespace dfy::runtime;
using namespace dfy::model;
using namespace dfy::plugins;
using namespace dfy::tests;
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
void scenario(const Manifest& manifest, const std::filesystem::path& settings, unsigned mode) {
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
    ScriptedModel model({1,"native_action_feedback",1,{},{generation,action,read,final},4}); Session session({},channel,model.backend()); ConfirmationCaller caller;
    RunOptions options; options.tools = scope; options.now = model.clock(); options.planning = PlanLimits{}; options.confirmation_caller = caller;
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
    check(assessment.match == (mode == 0 ? ObservationMatch::satisfied : mode == 1 ? ObservationMatch::not_satisfied : ObservationMatch::unknown),"native feedback confused target and evidence");
    if (mode) { check(request->operation()->state() == OperationState::pending,"model text completed an unverified target"); request->request_cancel(CancelReason::user); }
    until(session,[&] { return !session.busy(); }); model.verify_complete();
    check(count(0)-starts == 3 && count(1)-effects == 1 && count(2)-releases == 3 && request->tool_steps()[0].effect == ToolEffect::applied &&
        review_timeline(run_timeline(*request,"native-feedback")).run_exited,"feedback repeated action or lost actual exit");
    check(instance->shutdown() == ShutdownResult::closed,"feedback snapshots pinned native code");
}
}
// 只用固定模型响应，初读、变更和重读均调用真实测试 DLL。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 3,"expected package and settings"); const auto manifest = load_manifest(argv[1]);
        for (unsigned mode = 0; mode < 3; ++mode) scenario(manifest,argv[2],mode);
        std::cout << "Native action feedback checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
