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
// 验证未绑定、过期或缺乏实际完成工作的步骤确认被拒绝。
void rejects(const auto& action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid plan control accepted");
}
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
        for (unsigned repeat = 0; repeat < 3; ++repeat) request->plan();
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
struct Gate { unsigned calls = 0; bool release = false; };
class ReadWork final : public ToolWork {
  public:
    // 记录只读执行开始，结果与真实退出由测试分别控制。
    explicit ReadWork(std::shared_ptr<Gate> gate) : gate_(std::move(gate)) { ++gate_->calls; }
    // 返回固定结果，保留原资源直至测试放行。
    ToolProgress poll(CancelReason) override { return {ToolResult{true,ToolEffect::not_applied,"read value",ToolError::none},gate_->release}; }
  private:
    std::shared_ptr<Gate> gate_;
};
// 验证计划步骤使用原循环和工具子集，文本结束只进入宿主验证等待。
// 步骤：生成候选 → 执行有限只读步骤 → 分别检查工具结果与退出 → 等待验证或取消收尾。
void execution() {
    for (const bool cancel : {false,true}) {
        auto gate = std::make_shared<Gate>(); unsigned forbidden = 0; ToolCatalog catalog;
        const std::string schema = R"({"type":"object","properties":{},"additionalProperties":false})";
        catalog.add({"fixture/read",1,1,{"read","fixture",schema},false,true},[gate](const auto&) { return std::make_unique<ReadWork>(gate); });
        catalog.add({"fixture/other",1,1,{"other","fixture",schema},false,true},[&](const auto&) { ++forbidden; return completed_tool({true,ToolEffect::not_applied,"",ToolError::none}); });
        ScriptStep generation; generation.response = proposal();
        ScriptStep action; action.tool_names = {"read"}; action.response = Response{{Role::assistant,"",{{"call","read","{}"}}},FinishReason::tool_calls};
        ScriptStep answer; answer.last_role = Role::tool; answer.last_tool_id = "call"; answer.tool_names = {"read"}; answer.response = Response{{Role::assistant,"model claims complete"}};
        ScenarioSpec spec{1,"plan_step_execution",1,{},{generation,action},2}; if (!cancel) { spec.steps.push_back(answer); spec.expected_model_requests = 3; }
        ScriptedModel model(std::move(spec)); Session session({},channel(),model.backend()); RunOptions options;
        options.planning = PlanLimits{}; options.now = model.clock(); const std::vector<std::string> ids{"fixture/read","fixture/other"}; options.tools = catalog.snapshot(ids);
        const auto request = session.begin("host goal",options); until(session,[&] { return gate->calls == 1; });
        const auto active = request->plan()->steps[0];
        check(active.state == PlanStepState::running && active.result == OperationState::pending && active.exit == OperationState::pending &&
            active.execution->step.run == request->run_id() && active.execution->step.sequence == 2 && request->tool_steps()[0].exit == OperationState::pending,
            "plan step lost original work or forged exit");
        if (cancel) { request->request_cancel(CancelReason::user); session.advance(); }
        gate->release = true;
        if (cancel) until(session,[&] { return !session.busy(); });
        else {
            until(session,[&] { return request->plan()->steps[0].waiting == PlanWait::verification; });
            check(request->operation()->state() == OperationState::pending && session.message_count() == 5 && request->plan()->steps[0].state == PlanStepState::running,
                "model text completed plan or lost step history");
            for (unsigned repeat = 0; repeat < 3; ++repeat) session.advance();
            request->request_cancel(CancelReason::user); until(session,[&] { return !session.busy(); });
        }
        model.verify_complete(); check(gate->calls == 1 && !forbidden && request->plan()->stopped &&
            request->plan()->steps[0].exit == OperationState::succeeded,"plan step replayed or lost actual exit");
    }
}
// 验证模型越过步骤工具范围时零动作，规划生成也消耗原运行模型预算。
// 步骤：固定原范围及只读步骤 → 返回范围外候选或耗尽预算 → 检查拒绝事实和原运行退出。
void restricted_step() {
    for (const bool limited : {false,true}) {
        unsigned calls = 0; ToolCatalog catalog; register_read(catalog,calls);
        catalog.add({"fixture/other",1,1,{"other","fixture",R"({"type":"object","properties":{},"additionalProperties":false})"},false,true},
            [&](const auto&) { ++calls; return completed_tool({true,ToolEffect::not_applied,"",ToolError::none}); });
        ScriptStep generation; generation.response = proposal();
        ScriptStep action; action.tool_names = {"read"}; action.response = Response{{Role::assistant,"",{{"call","other","{}"}}},FinishReason::tool_calls};
        ScriptStep answer; answer.last_role = Role::tool; answer.tool_names = {"read"}; answer.response = Response{{Role::assistant,"rejected"}};
        ScenarioSpec spec{1,"restricted_plan_step",1,{},{generation},1};
        if (!limited) { spec.steps.push_back(action); spec.steps.push_back(answer); spec.expected_model_requests = 3; }
        ScriptedModel model(std::move(spec)); Session session({},channel(),model.backend()); RunOptions options;
        options.planning = PlanLimits{}; options.now = model.clock(); const std::vector<std::string> ids{"fixture/read","fixture/other"}; options.tools = catalog.snapshot(ids);
        if (limited) options.policy.model_limit = 1;
        const auto request = session.begin("host goal",options);
        if (limited) {
            until(session,[&] { return !session.busy(); });
            check(request->diagnostics().stop_reason == PolicyReason::model_limit && request->plan()->steps[0].state == PlanStepState::failed,
                "planning bypassed original model budget");
        } else {
            until(session,[&] { return request->plan()->definition && request->plan()->steps[0].waiting == PlanWait::verification; });
            check(!request->tool_steps()[0].accepted && request->tool_steps()[0].effect == ToolEffect::not_applied,"step scope allowed outside tool");
            request->request_cancel(CancelReason::user); until(session,[&] { return !session.busy(); });
        }
        model.verify_complete(); check(!calls,"restricted or over-budget step executed tool");
    }
}
// 验证宿主确认后的顺序推进、完成结算和幂等查询，不把调用者正文当作身份。
// 步骤：生成两步计划 → 拒绝错绑并确认首步 → 执行及确认第二步 → 终态重试保持零新增工作。
void verified_sequence() {
    ScriptStep generation; generation.response = Response{{Role::assistant,R"({"schema_version":1,"steps":[
        {"id":"first","objective":"first","completion_criterion":"host checks","tools":[]},
        {"id":"second","objective":"second","completion_criterion":"host checks","tools":[]}]})"}};
    ScriptStep first; first.response = Response{{Role::assistant,"first value"}};
    ScriptStep second; second.response = Response{{Role::assistant,"second value"}};
    ScriptedModel model({1,"verified_plan",1,{},{generation,first,second},3}); Session session({},channel(),model.backend());
    ConfirmationCaller caller, stranger; RunOptions options; options.planning = PlanLimits{}; options.now = model.clock(); options.confirmation_caller = caller;
    const auto request = session.begin("host goal",options);
    until(session,[&] { return request->plan()->definition && request->plan()->steps[0].waiting == PlanWait::verification; });
    const auto key = request->plan()->steps[0].key; auto wrong = key; ++wrong.plan.plan_revision;
    rejects([&] { session.confirm_plan_step(stranger,key,"checked"); });
    rejects([&] { session.confirm_plan_step(caller,wrong,"checked"); });
    rejects([&] { session.confirm_plan_step(caller,request->plan()->steps[1].key,"premature"); });
    session.confirm_plan_step(caller,key,"checked"); session.confirm_plan_step(caller,key,"checked");
    rejects([&] { session.confirm_plan_step(caller,key,"different evidence"); });
    check(request->diagnostics().model_requests == 2 && request->operation()->state() == OperationState::pending,"confirmation executed next step inline");
    until(session,[&] { return request->plan()->steps[1].waiting == PlanWait::verification; });
    session.confirm_plan_step(caller,key,"checked"); const auto last = request->plan()->steps[1].key;
    session.confirm_plan_step(caller,last,"last checked"); until(session,[&] { return !session.busy(); }); model.verify_complete();
    check(request->answer() == "second value" && session.message_count() == 5 && request->plan()->steps[0].state == PlanStepState::completed &&
        request->plan()->steps[1].state == PlanStepState::completed,"verified plan did not complete original run");
    session.confirm_plan_step(caller,last,"last checked"); rejects([&] { session.confirm_plan_step(stranger,last,"last checked"); });
    check(model.report().attempted == 3 && !session.busy(),"terminal verification restarted completed run");
}
// 验证验证等待期间的取消和期限使新确认失效，不允许确认命令恢复已停止工作。
// 步骤：等待纯文本步骤完成 → 注入停止 → 拒绝原确认 → 收尾并保留未验证状态。
void verification_stops() {
    for (const bool timeout : {false,true}) {
        ScriptStep generation; generation.response = Response{{Role::assistant,R"({"schema_version":1,"steps":[{"id":"one","objective":"one","completion_criterion":"host checks","tools":[]}]})"}};
        ScriptStep answer; answer.response = Response{{Role::assistant,"done"}};
        ScriptedModel model({1,"verification_stop",1,{},{generation,answer},2}); Session session({},channel(),model.backend()); ConfirmationCaller caller;
        RunOptions options; options.planning = PlanLimits{}; options.now = model.clock(); options.confirmation_caller = caller;
        const auto request = session.begin("host goal",options);
        until(session,[&] { return request->plan()->definition && request->plan()->steps[0].waiting == PlanWait::verification; }); const auto key = request->plan()->steps[0].key;
        if (timeout) model.advance_clock(options.timeout); else request->request_cancel(CancelReason::user);
        rejects([&] { session.confirm_plan_step(caller,key,"too late"); }); until(session,[&] { return !session.busy(); }); model.verify_complete();
        check(request->operation()->state() == OperationState::cancelled && request->plan()->steps[0].state == PlanStepState::running &&
            request->plan()->steps[0].waiting == PlanWait::stopped,"late verification resumed or completed stopped goal");
    }
}
}
// 只使用原模型替身验证正式规划生成及停止，不调用真实模型。
int main() {
    try { accepted(); failures_and_default(); execution(); restricted_step(); verified_sequence(); verification_stops(); std::cout << "Session planning checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
