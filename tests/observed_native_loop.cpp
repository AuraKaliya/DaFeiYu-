#include "support/scripted_model.hpp"
#include <dafeiyu/observation_controller.hpp>
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
// 报告真实原生观察触发链路与契约不符。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 用原 Session 推进到明确状态，控制器查询不代替真实退出。
void until(Session& session, const auto& predicate) {
    const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (!predicate() && std::chrono::steady_clock::now() < end) { session.advance(); std::this_thread::yield(); }
    check(predicate(),"observed native run stalled");
}
// 创建无设备的合成状态源，原生工具只用于验证实际调用和资源生命周期。
std::shared_ptr<const StructValue> payload() {
    auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("fixture.synthetic",1,{}, {1,64}));
    return std::make_shared<const StructValue>(StructValue::create(type,{},64));
}
// 验证观察→固定模型→原生只读工具→模型，以及来源撤销后的停止与收尾。
// 步骤：装配真实 DLL 和只读范围 → 显式触发 → 等待实际工具结果 → 正常完成或撤销来源 → 核对记录并卸载。
void scenario(const Manifest& manifest, const std::filesystem::path& settings, bool revoke) {
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(16,64),1,8);
    auto instance = std::make_shared<PluginInstance>(manifest,settings,channel); check(!instance->create() && !instance->start(),"native setup failed");
    auto catalog = std::make_shared<ToolCatalog>(); PluginTools registration(instance,catalog);
    const std::vector<std::string> ids{"test.tools/read"}; const auto tools = catalog->snapshot(ids); const auto name = tools.definitions()[0].name;
    ScriptStep first; first.tool_names = {name}; first.response = Response{{Role::assistant,"",{{"read",name,"{}"}}},FinishReason::tool_calls};
    ScriptStep last; last.last_role = Role::tool; last.last_tool_id = "read"; last.tool_names = {name};
    last.last_text = "status=succeeded; effect=not_applied; error=none; fault=none\noutput:\n0"; last.response = Response{{Role::assistant,"read complete"}};
    ScenarioSpec spec{1,"observed_native",1,{},{first,last},2}; if (revoke) { spec.steps.resize(1); spec.expected_model_requests = 1; }
    ScriptedModel model(std::move(spec)); Session session({},channel,model.backend());
    ObservationSource source(instance->generation(),ObservationTrust::external_data); ObservationInbox inbox({source.id()});
    inbox.accept(source.capture({"counter",1},1,{},ObservationTime{}+std::chrono::seconds(1),payload()),{});
    RunOptions options; options.now = model.clock(); options.tools = tools; options.policy.allow_observation = true;
    ObservationController controller(session,inbox,{source.id(),{"counter",1},1,1,1,std::chrono::milliseconds(10),ObservationTime{}+std::chrono::seconds(1)},"read state",options);
    controller.enable(); const auto request = controller.poll().request; check(static_cast<bool>(request),"observation did not start native run");
    until(session,[&] { const auto steps = request->tool_steps(); return steps.size() == 1 && steps[0].result == OperationState::succeeded; });
    check(request->tool_steps()[0].exit == OperationState::pending && instance->shutdown() == ShutdownResult::in_use,"native result forged release");
    if (revoke) {
        source.revoke(); check(controller.poll().decision == TriggerDecision::stale && request->operation()->cancellation_reason() == CancelReason::owner_stop,
            "source revocation did not stop owned run");
    }
    until(session,[&] { return request->exit()->state() == OperationState::succeeded; }); model.verify_complete();
    const auto trace = run_timeline(*request,"observed-native"); const auto report = review_timeline(trace);
    check(report.tools == 1 && report.run_exited && trace.source == RunOrigin::observation_rule && trace.observations[0].provider_instance == instance->generation() &&
        request->operation()->state() == (revoke ? OperationState::cancelled : OperationState::succeeded) && session.message_count() == (revoke ? 4u : 5u),
        "native observation facts or context lost");
    check(instance->shutdown() == ShutdownResult::closed,"retained trace or observation pinned native code");
}
}
// 原生 DLL、线程和会话均实际执行，模型响应始终来自固定替身。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 3,"expected package and settings"); const auto manifest = load_manifest(argv[1]);
        scenario(manifest,argv[2],false); scenario(manifest,argv[2],true); std::cout << "Observed native loop checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
