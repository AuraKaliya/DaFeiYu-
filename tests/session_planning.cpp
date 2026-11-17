#include "support/scripted_model.hpp"
#include <dafeiyu/session.hpp>
#include <dafeiyu/run_trace.hpp>
#include <iostream>
#include <thread>

namespace {
using namespace dfy::agent;
using namespace dfy::runtime;
using namespace dfy::model;
using namespace dfy::tests;
// 报告正式规划接纳、等待或退出边界与固定场景不符。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 建立无外部消费者的原生会话事件通道。
std::shared_ptr<dfy::events::BroadcastChannel> channel() {
    return std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
}
// 提供固定候选，正文不能自行改变宿主范围或执行工具。
Response proposal() {
    return {{Role::assistant,R"({"schema_version":1,"steps":[{"id":"read","objective":"读取状态","completion_criterion":"宿主验证回执","tools":["fixture/read"]}]})"}};
}
// 有界推进到指定可查询状态，不由查询本身驱动执行。
void until(Session& session, const auto& ready) {
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (!ready() && std::chrono::steady_clock::now() < deadline) { session.advance(); std::this_thread::yield(); }
    check(ready(),"session planning did not reach expected state");
}
// 注册本地只读计数工具，规划候选阶段不得调用它。
void register_read(ToolCatalog& catalog, unsigned& calls) {
    catalog.add({"fixture/read",1,1,{"read","fixture",R"({"type":"object","properties":{},"additionalProperties":false})"},false,true},
        [&](const auto&) { ++calls; return completed_tool({true,ToolEffect::not_applied,"value",ToolError::none}); });
}
// 验证规划请求沿用原运行与模型事实，候选就绪不结算目标或改变历史。
// 步骤：显式接纳规划 → 等待候选并只读查询 → 用户取消或期限停止 → 确认实际退出和零工具调用。
void accepted() {
    for (const bool timeout : {false,true}) {
        unsigned calls = 0; ToolCatalog catalog; register_read(catalog,calls); const std::vector<std::string> ids{"fixture/read"};
        ScriptStep step; step.response = proposal(); ScriptedModel model({1,"session_plan",1,{},{step},1});
        Session session({},channel(),model.backend()); RunOptions options; options.now = model.clock(); options.planning = PlanLimits{};
        options.tools = catalog.snapshot(ids); options.goal_revision = 7; options.timeout = std::chrono::milliseconds(10);
        const auto request = session.begin("host goal",options);
        until(session,[&] { return !request->model_steps().empty(); });
        const auto plan = request->plan(); const auto trace = run_timeline(*request,"plan-fixture");
        check(plan && plan->definition && plan->definition->binding() == PlanBinding{request->run_id(),7,1} &&
            plan->steps[0].state == PlanStepState::pending && request->operation()->state() == OperationState::pending &&
            request->exit()->state() == OperationState::pending && !session.message_count() && !calls && !review_timeline(trace).run_exited,
            "candidate finished run, executed tools or lost binding");
        for (unsigned repeat = 0; repeat < 3; ++repeat) { request->plan(); session.advance(); }
        check(request->diagnostics().model_requests == 1 && !calls,"query or pending plan restarted model");
        if (timeout) model.advance_clock(std::chrono::milliseconds(10)); else request->request_cancel(CancelReason::user);
        until(session,[&] { return !session.busy(); }); model.verify_complete();
        check(request->operation()->state() == OperationState::cancelled && request->plan()->stopped &&
            request->exit()->state() == OperationState::succeeded && !session.message_count() && !calls,"planning stop lost original lifecycle");
    }
}
// 验证候选错误和模型失败不能降级执行，普通问答仍沿用原文本路径。
// 步骤：逐类提交固定响应 → 收集原终态 → 核对零工具动作和历史兼容。
void failures_and_default() {
    for (unsigned mode = 0; mode < 4; ++mode) {
        unsigned calls = 0; ToolCatalog catalog; register_read(catalog,calls); const std::vector<std::string> ids{"fixture/read"};
        ScriptStep step;
        if (mode == 0) step.response = Response{{Role::assistant,"done"}};
        if (mode == 1) step.response = Response{{Role::assistant,"",{{"call","read","{}"}}},FinishReason::tool_calls};
        if (mode == 2) step.fail = true;
        if (mode == 3) { step.response = Response{{Role::assistant,"plain answer"}}; step.last_text = "host goal"; }
        ScriptedModel model({1,"plan_refused",1,{},{step},1}); Session session({},channel(),model.backend());
        RunOptions options; options.now = model.clock();
        if (mode != 3) { options.planning = PlanLimits{}; options.tools = catalog.snapshot(ids); }
        const auto request = session.begin("host goal",options); until(session,[&] { return !session.busy(); }); model.verify_complete();
        check(!calls && request->operation()->state() == (mode == 3 ? OperationState::succeeded : OperationState::failed),"invalid candidate executed or completed");
        if (mode == 3) check(!request->plan() && request->answer() == "plain answer" && session.message_count() == 2,"default text entered planning");
        else check(request->plan()->stopped && !request->plan()->definition && !session.message_count(),"failure installed a plan or history");
    }
}
}
// 只使用原模型替身验证正式规划生成及停止，不调用真实模型。
int main() {
    try { accepted(); failures_and_default(); std::cout << "Session planning checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
