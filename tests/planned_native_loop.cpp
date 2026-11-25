#include "../modules/agent/tests/support/scripted_model.hpp"
#include <dafeiyu/plugin_tools.hpp>
#include <dafeiyu/run_trace.hpp>
#include <iostream>
#include <thread>

namespace {
using namespace dfy::agent;
using namespace dfy::runtime;
using namespace dfy::model;
using namespace dfy::plugins;
using namespace dfy::tests;
enum class End { complete, cancel, replace, unload, destroy };
// 报告正式计划与真实 DLL 的执行、退出或权限错误。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 验证旧确认或尚未退出的目标更新明确拒绝。
void rejects(const auto& action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid planned native command accepted");
}
// 有界推进宿主，真实 DLL 的退出由原工作状态确认。
void until(Session& session, const auto& ready) {
    const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (!ready() && std::chrono::steady_clock::now() < end) { session.advance(); std::this_thread::yield(); }
    check(ready(),"planned native run stalled");
}
// 提供固定候选，原目标包含变更和读取，新目标仅保留读取。
Response proposal(bool read_only = false) {
    const std::string change = R"({"id":"change","objective":"increment once","completion_criterion":"host checks effect","tools":["test.tools/increment"]},)";
    return {{Role::assistant,std::string(R"({"schema_version":1,"steps":[)")+(read_only ? "" : change)+
        R"({"id":"read","objective":"read state","completion_criterion":"host checks value","tools":["test.tools/read"]}]})"}};
}
// 验证计划→确认→原生动作→宿主验证→后续步骤，并核对停止、卸载和目标更新。
// 步骤：固定候选及真实实例 → 确认前保持零动作 → 检查效果与实际退出 → 验证或替换目标 → 核对唯一效果及代码释放。
void scenario(const Manifest& manifest, const std::filesystem::path& settings, End ending) {
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(16,64),1,8);
    auto instance = std::make_shared<PluginInstance>(manifest,settings,channel); check(!instance->create() && !instance->start(),"native setup failed");
    auto catalog = std::make_shared<ToolCatalog>(); PluginTools registration(instance,catalog);
    const std::vector<std::string> ids{"test.tools/increment","test.tools/read"}; const auto scope = catalog->snapshot(ids,true);
    const auto change_name = scope.restrict_to(std::vector<std::string>{ids[0]}).definitions()[0].name;
    const auto read_name = scope.restrict_to(std::vector<std::string>{ids[1]}).definitions()[0].name;
    dfy::platform::DynamicLibrary counter(manifest.entry);
    const auto count = reinterpret_cast<uint32_t (DFY_V2_CALL*)(uint32_t)>(counter.symbol("dfy_tool_test_count"));
    const auto starts = count(0), effects = count(1), releases = count(2);
    ScriptStep generation; generation.response = proposal();
    ScriptStep change; change.tool_names = {change_name}; change.response = Response{{Role::assistant,"",{{"change",change_name,"{}"}}},FinishReason::tool_calls};
    ScriptStep changed; changed.last_role = Role::tool; changed.last_tool_id = "change";
    if (ending != End::unload) changed.tool_names = {change_name};
    changed.response = Response{{Role::assistant,"changed"}};
    ScriptStep read; read.tool_names = {read_name}; read.response = Response{{Role::assistant,"",{{"read",read_name,"{}"}}},FinishReason::tool_calls};
    ScriptStep final; final.last_role = Role::tool; final.last_tool_id = "read"; final.tool_names = {read_name};
    final.last_text = "status=succeeded; effect=not_applied; error=none; fault=none\noutput:\n1"; final.response = Response{{Role::assistant,"verified value"}};
    ScenarioSpec spec{1,"planned_native",1,{},{generation,change},2};
    if (ending != End::cancel && ending != End::destroy) spec.steps.push_back(changed);
    if (ending == End::replace) { ScriptStep next; next.response = proposal(true); spec.steps.push_back(next); }
    if (ending == End::complete || ending == End::replace) { spec.steps.push_back(read); spec.steps.push_back(final); }
    spec.expected_model_requests = spec.steps.size(); ScriptedModel model(std::move(spec));
    auto session = std::make_unique<Session>(Config{},channel,model.backend()); ConfirmationCaller caller; RunOptions options;
    options.planning = PlanLimits{}; options.tools = scope; options.now = model.clock(); options.confirmation_caller = caller; options.policy.confirm_changes = true;
    const auto request = session->begin("increment then read",options); until(*session,[&] { return !request->confirmations().empty(); });
    const auto original = request->plan()->definition->binding(); const auto first_key = request->plan()->steps[0].key;
    const auto pending = request->confirmations()[0]; ConfirmationCommand command{1,pending.id,pending.revision,pending.target,true};
    check(pending.target.plan_revision == 1 && pending.target.plan_step == 1 && pending.target.goal_revision == 1,"native confirmation lost plan binding");
    check(count(0) == starts,"plan bypassed confirmation"); rejects([&] { session->revise_goal(caller,original,"read instead"); });
    if (ending == End::unload) {
        check(instance->shutdown() == ShutdownResult::closed,"waiting plan pinned DLL"); registration.revoke();
        rejects([&] { session->decide(caller,command); });
    } else {
        const auto receipt = session->decide(caller,command); check(session->decide(caller,command) == receipt && count(0) == starts,"approval executed or repeated inline");
        until(*session,[&] { return count(1) != effects; });
        check(!request->tool_result(request->tool_steps()[0].candidate),"inflight native result published as exited feedback");
        check(request->tool_steps()[0].exit == OperationState::pending && instance->shutdown() == ShutdownResult::in_use,"effect forged native release");
        rejects([&] { session->confirm_plan_step(caller,first_key,"premature"); });
        rejects([&] { session->revise_goal(caller,original,"read instead"); });
    }
    if (ending == End::destroy) session.reset();
    else if (ending == End::cancel) request->request_cancel(CancelReason::user);
    else {
        until(*session,[&] { return request->plan()->steps[0].waiting == PlanWait::verification; });
        if (ending == End::unload) request->request_cancel(CancelReason::user);
        else {
            if (ending == End::replace) {
                session->revise_goal(caller,original,"read instead");
                check(request->plan_history()[0].steps[0].state == PlanStepState::running,"new goal forged old validation");
            } else session->confirm_plan_step(caller,first_key,"native effect checked");
            until(*session,[&] { const auto plan = request->plan(); return plan->definition && plan->steps.back().waiting == PlanWait::verification; });
            session->confirm_plan_step(caller,request->plan()->steps.back().key,"native value checked");
        }
    }
    if (session) until(*session,[&] { return !session->busy(); });
    else { Session::retry_retained(); check(request->exit()->state() == OperationState::succeeded,"destroyed session lost native exit"); }
    model.verify_complete(); const auto trace = run_timeline(*request,"planned-native"); check(review_timeline(trace).run_exited,"plan trace did not record actual exit");
    const auto value = request->tool_result(request->tool_steps()[0].candidate);
    check(!request->tool_result(0) && (ending == End::unload ? !value : value && value->output == "1" && value->effect == ToolEffect::applied),
        "tool feedback lost original native outcome or used model text");
    const auto expected_calls = ending == End::unload ? 0u : ending == End::complete || ending == End::replace ? 2u : 1u;
    check(count(0)-starts == expected_calls && count(2)-releases == expected_calls && count(1)-effects == (ending == End::unload ? 0u : 1u),
        "plan repeated effects or lost native release");
    check(request->operation()->state() == (ending == End::complete || ending == End::replace ? OperationState::succeeded : OperationState::cancelled) &&
        instance->shutdown() == ShutdownResult::closed,"plan outcome or retained snapshots blocked unload");
}
}
// 计划文本全部来自固定替身，原生实例、调用、确认和退出按实际链路验收。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 3,"expected package and settings"); const auto manifest = load_manifest(argv[1]);
        for (const auto ending : {End::complete,End::cancel,End::replace,End::unload,End::destroy}) scenario(manifest,argv[2],ending);
        std::cout << "Planned native loop checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
