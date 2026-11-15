#include "support/scripted_model.hpp"
#include <dafeiyu/run_loop.hpp>
#include <dafeiyu/session.hpp>
#include <iostream>
#include <thread>

namespace {
using namespace dfy::agent;
using namespace dfy::model;
using namespace dfy::runtime;
using namespace dfy::tests;
// 报告实际循环与预设场景不一致。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 有界推进协调器，测试时限不作为产品运行的停止机制。
void drain(RunLoop& loop) {
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (loop.request()->exit()->state() == OperationState::pending && std::chrono::steady_clock::now() < deadline) { loop.advance(); std::this_thread::yield(); }
    check(loop.request()->exit()->state() == OperationState::succeeded,"run did not exit");
}
// 创建固定工具响应与最终文本，不由模型选择实际工具。
ScenarioSpec script(bool failure = false, bool repeat = false) {
    ScriptStep first; first.last_text = "goal"; first.tool_names = {"action"};
    first.response = Response{{Role::assistant,"",{{"call","action","{}"}}},FinishReason::tool_calls};
    ScriptStep second; second.last_role = Role::tool; second.last_tool_id = "call"; second.tool_names = {"action"};
    if (failure) second.fail = true;
    else second.response = repeat ? first.response : std::optional<Response>(Response{{Role::assistant,"done"}});
    return {1,"tool_loop",1,{},{first,second},2};
}
// 注册本地计数夹具，不接触任何外部资源。
void register_tool(ToolCatalog& catalog, unsigned& calls, bool changes = true) {
    catalog.add({"fixture/action",1,1,{"action","fixture",R"({"type":"object","properties":{},"additionalProperties":false})"},changes,false},
        [&,changes](const auto&) { ++calls; return completed_tool({true,changes ? ToolEffect::applied : ToolEffect::not_applied,"value",ToolError::none}); });
}
// 验证正式控制规则下模型与工具循环，并保留后续模型失败前已经发生的效果。
// 步骤：分别运行成功及失败序列 → 核对步骤和调用次数 → 检查动作事实与上下文。
void outcomes() {
    for (const bool failure : {false,true}) {
        unsigned calls = 0; ToolCatalog catalog; register_tool(catalog,calls); const std::vector<std::string> ids{"fixture/action"};
        ScriptedModel model(script(failure)); RunOptions options; options.tools = catalog.snapshot(ids,true); options.now = model.clock();
        RunLoop loop({1,1},{},model.backend(),std::make_shared<const MessageHistory>(),"goal",options); drain(loop); model.verify_complete();
        const auto request = loop.request();
        check(calls == 1 && request->diagnostics().model_requests == 2 && request->model_steps().size() == 2 && request->step_id().sequence == 2 &&
            request->tool_steps()[0].effect == ToolEffect::applied,"loop lost step association or invoked extra action");
        check(request->operation()->state() == (failure ? OperationState::failed : OperationState::succeeded) &&
            loop.history()->size() == (failure ? 3u : 4u) && (*loop.history())[2].content.find("effect=applied") != std::string::npos,
            "model failure erased action facts or final answer was not committed");
    }
}
// 验证模型次数和无进展边界在下一调用前停止，不依赖真实服务失败。
// 步骤：准备各限额的固定序列 → 推进到终态 → 核对停止原因和真实调用次数。
void limits() {
    const std::vector<std::string> ids{"fixture/action"};
    for (const bool repeat : {false,true}) {
        unsigned calls = 0; ToolCatalog catalog; register_tool(catalog,calls,false); auto spec = script(false,repeat);
        if (!repeat) { spec.steps.resize(1); spec.expected_model_requests = 1; }
        ScriptedModel model(std::move(spec)); RunOptions options; options.tools = catalog.snapshot(ids); options.now = model.clock();
        options.policy.model_limit = repeat ? 8 : 1; options.no_progress_limit = 1;
        RunLoop loop({1,1},{},model.backend(),std::make_shared<const MessageHistory>(),"goal",options); drain(loop); model.verify_complete();
        check(loop.request()->operation()->state() == OperationState::failed && loop.request()->diagnostics().stop_reason ==
            (repeat ? PolicyReason::no_progress : PolicyReason::model_limit) && calls == (repeat ? 2u : 1u),"run limit failed");
    }
}
// 验证模型前控制、运行期限和整批调用上限都在受控动作之前生效。
// 步骤：拒绝模型前异常 → 模拟到期与超量批次 → 核对零动作及原历史。
void admission_limits() {
    ScenarioSpec empty{1,"no_model",1,{}, {},0}; ScriptedModel no_model(empty); RunOptions denied;
    denied.extension = [](auto,const auto&) -> PolicyDecision { throw std::runtime_error("controlled rule failure"); };
    bool rejected = false;
    try { RunLoop loop({1,1},{},no_model.backend(),std::make_shared<const MessageHistory>(),"goal",denied); }
    catch (const std::runtime_error&) { rejected = true; }
    check(rejected,"model control exception allowed admission"); no_model.verify_complete();
    for (const bool timeout : {false,true}) {
        unsigned calls = 0; ToolCatalog catalog; register_tool(catalog,calls); const std::vector<std::string> ids{"fixture/action"};
        auto spec = script(); spec.steps.resize(1); spec.expected_model_requests = 1;
        if (timeout) spec.steps[0].advance_by = std::chrono::milliseconds(5);
        else spec.steps[0].response->message.tool_calls.push_back({"second","action","{}"});
        ScriptedModel model(std::move(spec)); RunOptions options; options.tools = catalog.snapshot(ids,true); options.now = model.clock();
        if (timeout) options.timeout = std::chrono::milliseconds(5); else options.policy.tool_limit = 1;
        RunLoop loop({1,1},{},model.backend(),std::make_shared<const MessageHistory>(),"goal",options); drain(loop); model.verify_complete();
        check(!calls && loop.request()->diagnostics().stop_reason == (timeout ? PolicyReason::deadline : PolicyReason::tool_limit) &&
            loop.history()->empty(),"expired or oversized batch applied effects");
    }
}
struct Gate { bool release = false; unsigned effects = 0; };
class LateWork final : public ToolWork {
  public:
    // 保留可控异步状态，供验证取消后实际退出。
    explicit LateWork(std::shared_ptr<Gate> gate) : gate_(std::move(gate)) { ++gate_->effects; }
    // 结果立即可见，资源释放由测试明确放行。
    ToolProgress poll(CancelReason) override { return {ToolResult{true,ToolEffect::applied,"late",ToolError::none},gate_->release}; }
  private:
    std::shared_ptr<Gate> gate_;
};
// 验证协调器离开作用域后保留实际工作，取消不丢失效果或恢复模型调用。
// 步骤：开始可控工作 → 请求取消并销毁协调器 → 放行原工作 → 重试并核对唯一退出。
void retention() {
    auto gate = std::make_shared<Gate>(); ToolCatalog catalog;
    catalog.add({"fixture/action",1,1,{"action","fixture",R"({"type":"object","properties":{},"additionalProperties":false})"},true,false},
        [gate](const auto&) { return std::make_unique<LateWork>(gate); });
    auto spec = script(); spec.steps.resize(1); spec.expected_model_requests = 1; ScriptedModel model(std::move(spec));
    const std::vector<std::string> ids{"fixture/action"}; RunOptions options; options.tools = catalog.snapshot(ids,true); options.now = model.clock();
    std::shared_ptr<TurnRequest> request;
    {
        RunLoop loop({1,1},{},model.backend(),std::make_shared<const MessageHistory>(),"goal",options); request = loop.request();
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while (!gate->effects && std::chrono::steady_clock::now() < deadline) { loop.advance(); std::this_thread::yield(); }
        check(gate->effects == 1,"late work did not start"); request->request_cancel(CancelReason::user); loop.advance();
        check(request->operation()->state() == OperationState::cancelled && request->exit()->state() == OperationState::pending,"cancel forged exit");
    }
    check(RunLoop::retained() == 1,"unfinished run lost its host owner"); gate->release = true;
    check(!RunLoop::retry_retained() && request->exit()->state() == OperationState::succeeded && request->tool_steps()[0].effect == ToolEffect::applied &&
        gate->effects == 1,"retained run replayed or erased effects"); model.verify_complete();
}
}
// 用固定响应及本地工具验证完整循环，不调用真实模型。
int main() {
    try { outcomes(); limits(); admission_limits(); retention(); std::cout << "Run loop checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
