#include "../modules/agent/tests/support/scripted_model.hpp"
#include <dafeiyu/plugin_tools.hpp>
#include <dafeiyu/session.hpp>
#include <atomic>
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
// 报告正式会话与原生工具链路不一致。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 只推进正式会话，实际退出必须由底层工作确认。
void drain(Session& session, const std::shared_ptr<TurnRequest>& request) {
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (request->exit()->state() == OperationState::pending && std::chrono::steady_clock::now() < deadline) { session.advance(); std::this_thread::yield(); }
    check(request->exit()->state() == OperationState::succeeded,"formal session did not exit");
}
// 经正式 Session、模型替身、注册器和真实 DLL 完成多步，并验证失败后的下一轮仍看到动作事实。
// 步骤：加载工具及授权 → 运行成功或失败响应序列 → 查询步骤和历史 → 验证后续上下文并卸载。
void native_case(const Manifest& manifest, const std::filesystem::path& settings, bool failure) {
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(24,64),2,8);
    auto instance = std::make_shared<PluginInstance>(manifest,settings,channel); check(!instance->create() && !instance->start(),"native setup failed");
    auto catalog = std::make_shared<ToolCatalog>(); PluginTools registration(instance,catalog);
    const std::vector<std::string> ids{"test.tools/increment"}; const auto scope = catalog->snapshot(ids,true); const auto name = scope.definitions()[0].name;
    ScriptStep first; first.last_text = "goal"; first.tool_names = {name}; first.response = Response{{Role::assistant,"",{{"call",name,"{}"}}},FinishReason::tool_calls};
    ScriptStep second; second.last_role = Role::tool; second.last_tool_id = "call"; second.tool_names = {name};
    if (failure) second.fail = true; else second.response = Response{{Role::assistant,"done"}};
    ScenarioSpec spec{1,"native_session",1,{},{first,second},2};
    if (failure) { ScriptStep next; next.last_text = "follow"; next.response = Response{{Role::assistant,"summary"}}; spec.steps.push_back(next); spec.expected_model_requests = 3; }
    ScriptedModel model(std::move(spec)); const auto fixed = model.backend(); std::atomic<bool> saw_effect = false;
    Backend backend{[&](const Config& config,const Request& input) {
        if (input.messages.back().content == "follow") for (const auto& message : input.messages)
            if (message.role == Role::tool && message.content.find("effect=applied") != std::string::npos) saw_effect = true;
        return fixed.complete(config,input);
    }};
    Session session({},channel,std::move(backend)); RunOptions options; options.tools = scope; options.now = model.clock();
    auto request = session.begin("goal",options); drain(session,request);
    check(request->operation()->state() == (failure ? OperationState::failed : OperationState::succeeded) &&
        request->model_steps().size() == 2 && request->tool_steps().size() == 1 && request->tool_steps()[0].effect == ToolEffect::applied &&
        session.message_count() == (failure ? 3u : 4u),"formal native loop lost result, history or association");
    check(request->diagnostics().origin == request->model_steps()[0].call.submitted &&
        request->diagnostics().model.submitted >= request->diagnostics().origin,"whole-run elapsed origin moved to later model");
    if (failure) { auto next = session.begin("follow"); drain(session,next); check(saw_effect && next->answer() == "summary","later question forgot applied action"); }
    model.verify_complete(); check(instance->shutdown() == ShutdownResult::closed,"retained run result pinned native code");
}
// 验证控制回调关闭会话时尚未发起模型，不能重入接纳或清空历史。
void admission_close() {
    ScriptedModel model({1,"close_before_model",1,{}, {},0});
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    Session session({},channel,model.backend()); RunOptions options;
    options.extension = [&](auto,const auto&) { session.close(); return PolicyDecision{PolicyVerdict::allow,PolicyReason::none}; };
    bool rejected = false; try { (void)session.begin("goal",options); } catch (const std::runtime_error&) { rejected = true; }
    check(rejected && session.closed() && !session.message_count(),"closed admission submitted a model"); model.verify_complete();
}
// 验证首个请求提交前到期不接纳半个运行，之后正常请求仍使用原回合号。
// 步骤：在控制回调推进时钟 → 拒绝到期接纳 → 正常请求验证序号及唯一模型调用。
void preflight_deadline() {
    ScriptStep step; step.last_text = "goal"; step.response = Response{{Role::assistant,"done"}};
    ScriptedModel model({1,"preflight_deadline",1,{},{step},1});
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    Session session({},channel,model.backend()); auto clock = std::make_shared<ToolTime>(ToolTime{}); RunOptions options;
    options.timeout = std::chrono::milliseconds(1); options.now = [clock] { return *clock; };
    options.extension = [clock](auto,const auto&) { *clock += std::chrono::milliseconds(1); return PolicyDecision{PolicyVerdict::allow,PolicyReason::none}; };
    bool rejected = false; try { (void)session.begin("goal",options); } catch (const std::runtime_error&) { rejected = true; }
    check(rejected && !session.busy() && !model.report().attempted,"expired preflight consumed a model request");
    auto request = session.begin("goal"); drain(session,request);
    check(request->turn() == 1 && request->answer() == "done","rejected preflight consumed a turn"); model.verify_complete();
}
// 验证结果回调内取消仍阻止下一模型请求，同时保留已发生动作。
// 步骤：完成一次受控动作 → 回调内请求取消 → 验证终态、事实及模型次数。
void callback_cancel() {
    ToolCatalog catalog; unsigned actions = 0;
    catalog.add({"fixture/action",1,1,{"action","fixture",R"({"type":"object","properties":{},"additionalProperties":false})"},true,false},
        [&](const auto&) { ++actions; return completed_tool({true,ToolEffect::applied,"done",ToolError::none}); });
    ScriptStep step; step.last_text = "goal"; step.tool_names = {"action"}; step.response = Response{{Role::assistant,"",{{"call","action","{}"}}},FinishReason::tool_calls};
    ScriptedModel model({1,"callback_cancel",1,{},{step},1}); const std::vector<std::string> ids{"fixture/action"};
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    Session session({},channel,model.backend()); std::shared_ptr<TurnRequest> request; RunOptions options;
    options.tools = catalog.snapshot(ids,true); options.now = model.clock();
    options.extension = [&](auto point,const auto&) { if (point == ControlPoint::tool_result) request->request_cancel(CancelReason::user); return PolicyDecision{PolicyVerdict::allow,PolicyReason::none}; };
    request = session.begin("goal",options); drain(session,request); model.verify_complete();
    check(actions == 1 && request->operation()->state() == OperationState::cancelled && session.message_count() == 3 &&
        request->tool_steps()[0].effect == ToolEffect::applied,"callback cancellation allowed another model or lost action");
}
struct Gate { bool released = false; };
class PendingWork final : public ToolWork {
  public:
    // 保留控制收尾时刻的宿主夹具。
    explicit PendingWork(std::shared_ptr<Gate> gate) : gate_(std::move(gate)) {}
    // 返回已发生效果，实际退出由夹具放行。
    ToolProgress poll(CancelReason) override { return {ToolResult{true,ToolEffect::applied,"value",ToolError::none},gate_->released}; }
  private:
    std::shared_ptr<Gate> gate_;
};
// 在真实工具结果已成立但尚未退出时接收用户补充，核对整批提交顺序与原授权。
// 步骤：启动双调用批次 → 在工具收尾时追加 → 按场景取消或放行 → 核对实际请求和动作效果。
void tool_supplements(InputState expected) {
    auto gate = std::make_shared<Gate>();
    ToolCatalog catalog;
    catalog.add({"fixture/pending",1,1,{"pending","fixture",R"({"type":"object","properties":{},"additionalProperties":false})"},true,false},
        [gate](const auto&) { return std::make_unique<PendingWork>(gate); });
    const std::vector<std::string> ids{"fixture/pending"};
    auto scope = catalog.snapshot(ids,true);
    const auto name = scope.definitions().at(0).name;
    std::vector<Request> received;
    Backend backend{[&](const auto&,const Request& input) -> Response {
        received.push_back(input);
        if (received.size()==1) return {{Role::assistant,"",{{"one",name,"{}"},{"two",name,"{}"}}},FinishReason::tool_calls};
        return {{Role::assistant,"工具后的答复"}};
    }};
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(16,64),1,8);
    Session session({},channel,std::move(backend));
    RunOptions options; options.tools = scope; options.accept_user_input = true;
    if (expected==InputState::limit) options.policy.model_limit = 1;
    const auto request = session.begin("原授权",options);
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (std::chrono::steady_clock::now()<deadline) {
        session.advance();
        const auto tools = request->tool_steps();
        if (!tools.empty() && tools.front().result!=OperationState::pending) break;
        std::this_thread::yield();
    }
    const auto working = request->tool_steps();
    check(!working.empty() && working.front().effect==ToolEffect::applied && working.front().exit==OperationState::pending,
        "tool effect was not in flight during supplement");
    check(session.append_input(request->run_id(),"补充一").message==1 && session.append_input(request->run_id(),"补充二").message==2,
        "in-flight tool rejected supplements");
    if (expected==InputState::cancelled) request->request_cancel(CancelReason::user);
    session.advance();
    check(request->diagnostics().model_requests==1 && !request->inputs().front().model_step,"input entered a partial tool batch");
    gate->released = true;
    drain(session,request);
    for (const auto& input : request->inputs()) check(input.state==expected,"tool-boundary supplement got wrong outcome");
    check(request->tool_steps().front().effect==ToolEffect::applied,"supplement settlement lost applied effect");
    if (expected!=InputState::submitted) {
        check(received.size()==1 && session.message_count()>=3,"stopped continuation lost tool facts or made another model request");
        return;
    }

    check(received.size()==2 && received.back().messages.size()==6 && received.back().tools.size()==1 &&
        received.back().tools.front().name==name,"supplement changed original tool scope or call count");
    const auto& messages = received.back().messages;
    check(messages[1].tool_calls.size()==2 && messages[2].role==Role::tool && messages[2].tool_call_id=="one" &&
        messages[3].role==Role::tool && messages[3].tool_call_id=="two" && messages[4].role==Role::user && messages[4].content=="补充一" &&
        messages[5].role==Role::user && messages[5].content=="补充二" && request->inputs()[1].model_step==2 &&
        request->answer()=="工具后的答复","tool pair was split or a supplement was reordered");
}

// 验证 Session 关闭后的责任归属，以及模型结束与工具收尾的独立展示。
// 步骤：进入工具阶段 → 查询模型和运行退出 → 销毁会话 → 放行并重试保留运行。
void session_retention() {
    auto gate = std::make_shared<Gate>(); ToolCatalog catalog;
    catalog.add({"fixture/pending",1,1,{"pending","fixture",R"({"type":"object","properties":{},"additionalProperties":false})"},true,false},
        [gate](const auto&) { return std::make_unique<PendingWork>(gate); });
    const std::vector<std::string> ids{"fixture/pending"}; const auto scope = catalog.snapshot(ids,true);
    ScriptStep step; step.last_text = "goal"; step.tool_names = {"pending"}; step.response = Response{{Role::assistant,"",{{"call","pending","{}"}}},FinishReason::tool_calls};
    ScriptedModel model({1,"session_retention",1,{},{step},1}); std::shared_ptr<TurnRequest> request;
    {
        auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
        Session session({},channel,model.backend()); RunOptions options; options.tools = scope; options.now = model.clock(); request = session.begin("goal",options);
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while ((request->tool_steps().empty() || request->tool_steps()[0].result == OperationState::pending) && std::chrono::steady_clock::now() < deadline) {
            session.advance(); std::this_thread::yield();
        }
        check(!request->tool_steps().empty() && request->tool_steps()[0].effect == ToolEffect::applied,"pending tool did not start");
        std::ostringstream output; dfy::app::write_turn_status(output,request->diagnostics());
        check(output.str().find("Model call=returned, exit=confirmed") != std::string::npos && output.str().find("Run exit=waiting") != std::string::npos,
            "tool cleanup was displayed as an active model call");
    }
    check(Session::retained() == 1 && request->operation()->state() == OperationState::cancelled && request->exit()->state() == OperationState::pending,
        "session destruction dropped real cleanup responsibility");
    gate->released = true;
    check(!Session::retry_retained() && request->exit()->state() == OperationState::succeeded && request->tool_steps()[0].effect == ToolEffect::applied,
        "session cleanup lost late effect or exit"); model.verify_complete();
}
}
// 使用正式会话与真实 C 工具，但所有模型结果都来自固定序列。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 3,"expected package and settings"); const auto manifest = load_manifest(argv[1]);
        native_case(manifest,argv[2],false); native_case(manifest,argv[2],true); admission_close(); preflight_deadline(); callback_cancel(); session_retention();
        tool_supplements(InputState::submitted); tool_supplements(InputState::cancelled); tool_supplements(InputState::limit);
        std::cout << "Session native tool loop checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
