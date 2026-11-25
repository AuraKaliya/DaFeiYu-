#include "support/scripted_model.hpp"
#include <dafeiyu/plan_protocol.hpp>
#include <dafeiyu/plan_book.hpp>
#include <iostream>
#include <thread>

namespace {
using namespace dfy::agent;
using namespace dfy::model;
using namespace dfy::runtime;
using namespace dfy::tests;
// 报告候选解析或显式模型规划契约不一致。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 确认损坏、越权和旧版本候选拒绝，不降级猜测或执行动作。
void rejects(const auto& action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid plan protocol input accepted");
}
// 提供固定文本候选，不依赖真实模型规划表现。
Response proposal() { return {{Role::assistant,R"({"schema_version":1,"steps":[{"id":"read","objective":"读取状态","completion_criterion":"宿主验证回执","tools":["fixture/read"]}]})"}}; }
// 验证显式规划复用原模型线程与退出，候选不会执行工具或自行完成计划。
// 步骤：固定范围和规划输入 → 提交原异步模型替身 → 等待实际退出 → 解析并安装待执行候选。
void generation() {
    ToolCatalog catalog; unsigned calls = 0;
    catalog.add({"fixture/read",1,1,{"read","fixture",R"({"type":"object","properties":{},"additionalProperties":false})"},false,true},
        [&](const auto&) { ++calls; return completed_tool({true,ToolEffect::not_applied,"",ToolError::none}); });
    const std::vector<std::string> ids{"fixture/read"}; PlanScope scope({1,1},3,catalog.snapshot(ids));
    auto input = prepare_plan_prompt(scope,0,"host goal"); check(input.request.tools.empty() && input.request.messages[0].role == Role::system,"planning exposed execution tools");
    ScriptStep step; step.response = proposal(); ScriptedModel model({1,"plan_generation",1,{},{step},1});
    AsyncClient client(1,model.backend()); const auto request = client.submit_request({},input.request);
    const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (request->exit()->state() == OperationState::pending && std::chrono::steady_clock::now() < end) { client.advance(); std::this_thread::yield(); }
    check(request->exit()->state() == OperationState::succeeded,"planning model did not exit"); model.verify_complete();
    const auto definition = parse_plan_response(scope,input.expected,request->response()); PlanBook book(scope); book.install(input.expected,definition.steps());
    check(!calls && book.snapshot().steps[0].state == PlanStepState::pending && definition.binding() == PlanBinding{{1,1},3,1},"model candidate executed or changed goal");
    const auto next = prepare_plan_prompt(scope,1,"host goal",&definition); const auto revised = parse_plan_response(scope,next.expected,proposal());
    book.install(next.expected,revised.steps()); rejects([&] { book.install(next.expected,revised.steps()); });
    rejects([&] { prepare_plan_prompt(scope,1,"host goal"); });
}
// 验证格式、完成标志、权限和身份绑定，不把模型正文中的状态当成事实。
// 步骤：拒绝越权及损坏格式 → 拒绝自报状态及修订 → 拒绝截断、工具调用和旧目标。
void invalid() {
    PlanScope empty({1,1},1); const PlanBinding expected{{1,1},1,0};
    const auto parse = [&](std::string text) { parse_plan_response(empty,expected,Response{{Role::assistant,std::move(text)}}); };
    rejects([&] { parse(proposal().message.content); });
    rejects([&] { parse(R"({"schema_version":2,"steps":[]})"); });
    rejects([&] { parse(R"({"schema_version":1,"schema_version":1,"steps":[]})"); });
    rejects([&] { parse(R"({"schema_version":1,"steps":[],"goal_revision":99})"); });
    rejects([&] { parse(R"({"schema_version":1,"steps":[{"id":"x","objective":"goal","completion_criterion":"done","tools":[],"status":"completed"}]})"); });
    rejects([&] { parse("```json\n{}\n```"); }); rejects([&] { parse(std::string(64*1024+1,' ')); });
    Response cut{{Role::assistant,"{}"},FinishReason::length}; rejects([&] { parse_plan_response(empty,expected,cut); });
    Response tool{{Role::assistant,"",{{"call","read","{}"}}},FinishReason::tool_calls}; rejects([&] { parse_plan_response(empty,expected,tool); });
    auto foreign = expected; ++foreign.goal_revision; rejects([&] { parse_plan_response(empty,foreign,proposal()); });
}
}
// 只使用固定响应验证模型规划入口，不调用真实 Provider 或执行计划。
int main() {
    try { generation(); invalid(); std::cout << "Plan protocol checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
