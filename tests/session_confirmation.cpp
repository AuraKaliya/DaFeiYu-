#include "support/scripted_model.hpp"
#include <dafeiyu/plugin_tools.hpp>
#include <dafeiyu/session.hpp>
#include <iostream>
#include <thread>
#include "diagnostics.hpp"
#include <sstream>

namespace {
using namespace dfy::agent;
using namespace dfy::model;
using namespace dfy::runtime;
using namespace dfy::plugins;
using namespace dfy::tests;
// 报告正式确认流程违反执行或资源约束。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 验证无效决定明确拒绝，不接受静默忽略为成功。
void rejects(const auto& action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid session decision accepted");
}
// 有界推进直到测试所需状态，超时只报告测试失败。
void until(Session& session, const auto& condition) {
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (!condition() && std::chrono::steady_clock::now() < deadline) { session.advance(); std::this_thread::yield(); }
    check(condition(),"session confirmation did not reach expected state");
}
// 用真实 C DLL 验证等待、批准、拒绝、取消、到期及卸载，不调用模型服务。
// 步骤：建立固定模型序列及原生实例 → 等待并查询 → 提交决定或停止 → 核对唯一动作和资源退出。
void native(const Manifest& manifest, const std::filesystem::path& settings, unsigned mode) {
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(16,64),2,8);
    auto instance = std::make_shared<PluginInstance>(manifest,settings,channel);
    check(!instance->create() && !instance->start(),"native setup failed");
    auto catalog = std::make_shared<ToolCatalog>(); PluginTools registration(instance,catalog);
    const std::vector<std::string> ids{"test.tools/increment"}; const auto scope = catalog->snapshot(ids,true); const auto name = scope.definitions()[0].name;
    dfy::platform::DynamicLibrary counter(manifest.entry);
    const auto count = reinterpret_cast<uint32_t (DFY_V2_CALL*)(uint32_t)>(counter.symbol("dfy_tool_test_count")); const auto starts = count(0), effects = count(1);
    ScriptStep first; first.last_text = "goal"; first.tool_names = {name}; first.response = Response{{Role::assistant,"",{{"call",name,"{}"}}},FinishReason::tool_calls};
    ScriptStep last; last.last_role = Role::tool; last.last_tool_id = "call"; last.tool_names = mode == 4 ? std::vector<std::string>{} : std::vector<std::string>{name};
    last.response = Response{{Role::assistant,"done"}}; ScenarioSpec spec{1,"native_confirmation",1,{},{first,last},2};
    if (mode == 2 || mode == 3) { spec.steps.resize(1); spec.expected_model_requests = 1; }
    ScriptedModel model(std::move(spec)); Session session({},channel,model.backend()); ConfirmationCaller caller, stranger; RunOptions options;
    options.tools = scope; options.now = model.clock(); options.confirmation_caller = caller; options.policy.confirm_changes = true;
    const auto request = session.begin("goal",options); until(session,[&] { return !request->confirmations().empty(); });
    for (unsigned i = 0; i < 4; ++i) session.advance();
    check(count(0) == starts && request->operation()->state() == OperationState::pending && request->tool_steps()[0].confirmation_state == ConfirmationState::waiting,
        "waiting executed native tool or settled run");
    std::ostringstream status; dfy::app::write_turn_status(status,request->diagnostics());
    check(status.str().find("Confirmations=1; waiting=1") != std::string::npos && status.str().find("goal") == std::string::npos &&
        status.str().find(name) == std::string::npos,"waiting status missing or leaked business text");
    const auto value = request->confirmations().front(); ConfirmationCommand command{1,value.id,value.revision,value.target,mode != 1};
    rejects([&] { session.decide(stranger,command); });
    auto modified = command; modified.expected.arguments = R"({"changed":true})"; rejects([&] { session.decide(caller,modified); });
    std::optional<ConfirmationReceipt> receipt;
    if (mode == 2) { request->request_cancel(CancelReason::user); rejects([&] { session.decide(caller,command); }); }
    else if (mode == 3) { model.advance_clock(options.timeout); rejects([&] { session.decide(caller,command); }); }
    else if (mode == 4) {
        check(instance->shutdown() == ShutdownResult::closed,"waiting pinned native instance"); registration.revoke();
        rejects([&] { session.decide(caller,command); });
    } else { receipt = session.decide(caller,command); check(session.decide(caller,command) == *receipt,"decision retry changed receipt"); }
    check(count(0) == starts,"decision command executed action inline");
    until(session,[&] { return request->exit()->state() == OperationState::succeeded; }); model.verify_complete();
    check(count(0)-starts == (mode == 0 ? 1u : 0u) && count(1)-effects == (mode == 0 ? 1u : 0u),"confirmation repeated or bypassed action");
    check(request->operation()->state() == (mode == 2 || mode == 3 ? OperationState::cancelled : OperationState::succeeded),"confirmation corrupted run outcome");
    check(!request->diagnostics().waiting_confirmations && request->diagnostics().confirmation_requests == 1,"settled run still waiting for decision");
    if (receipt) check(session.decide(caller,command) == *receipt && count(0)-starts == (mode == 0 ? 1u : 0u),"completed receipt replayed action");
    check(instance->shutdown() == ShutdownResult::closed,"confirmation result pinned native instance");
}
}
// 只使用固定模型序列与真实测试 DLL，确认入口不依赖 GUI。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 3,"expected package and settings"); const auto manifest = load_manifest(argv[1]);
        for (unsigned mode = 0; mode < 5; ++mode) native(manifest,argv[2],mode);
        std::cout << "Session confirmation checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
