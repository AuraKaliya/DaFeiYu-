#include "support/scripted_model.hpp"
#include <dafeiyu/session.hpp>
#include <algorithm>
#include <iostream>
#include <thread>

namespace {
using namespace dfy::agent;
using namespace dfy::runtime;
using namespace dfy::model;
using namespace dfy::tests;
// 报告运行事实、投影状态和业务操作不一致。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 用固定响应验证多次模型与历史提交的独立事实，裁剪只影响诊断完整性。
// 步骤：准备成功、失败或取消序列 → 推进正式运行 → 核对业务及原事实 → 验证只读增量。
void scenario(unsigned mode) {
    ToolCatalog catalog; unsigned actions = 0;
    catalog.add({"fixture/action",1,1,{"action","fixture",R"({"type":"object","properties":{},"additionalProperties":false})"},true,false},
        [&](const auto&) { ++actions; return completed_tool({true,ToolEffect::applied,"private output",ToolError::none}); });
    const std::vector<std::string> ids{"fixture/action"};
    ScriptStep first; first.last_text = "private goal"; first.tool_names = {"action"}; first.response = Response{{Role::assistant,"",{{"call","action","{}"}}},FinishReason::tool_calls};
    ScriptStep last; last.last_role = Role::tool; last.last_tool_id = "call"; last.tool_names = {"action"};
    if (mode == 1) last.fail = true; else last.response = Response{{Role::assistant,"private answer"}};
    ScenarioSpec spec{1,"session_journal",1,{},{first,last},2}; if (mode == 2) { spec.steps.resize(1); spec.expected_model_requests = 1; }
    ScriptedModel model(std::move(spec)); auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(1,64),1,1);
    Session session({},channel,model.backend()); RunOptions options; options.tools = catalog.snapshot(ids,true); options.now = model.clock();
    if (mode == 3) options.trace_capacity = 2;
    std::shared_ptr<TurnRequest> request;
    if (mode == 2) options.extension = [&](auto point,const auto&) {
        if (point == ControlPoint::tool_result) request->request_cancel(CancelReason::user);
        return PolicyDecision{PolicyVerdict::allow,PolicyReason::none};
    };
    request = session.begin("private goal",options);
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (request->exit()->state() == OperationState::pending && std::chrono::steady_clock::now() < deadline) { session.advance(); std::this_thread::yield(); }
    model.verify_complete(); const auto delta = request->trace_delta();
    check(actions == 1 && request->exit()->state() == OperationState::succeeded && !delta.projection_failed,"trace changed execution or lost exit");
    const auto expected = mode == 1 ? OperationState::failed : mode == 2 ? OperationState::cancelled : OperationState::succeeded;
    check(request->operation()->state() == expected && delta.facts.back().kind == TraceFactKind::exited && delta.facts.back().result == expected,
        "terminal projection differs from original operation");
    if (mode == 3) check(delta.gap && delta.facts.size() == 2,"bounded projection hid its gap");
    else {
        const auto count = [&](TraceFactKind kind) { return std::count_if(delta.facts.begin(),delta.facts.end(),[&](const auto& fact) { return fact.kind == kind; }); };
        check(!delta.gap && count(TraceFactKind::model_submitted) == (mode == 2 ? 1 : 2) &&
            count(TraceFactKind::model_exited) == (mode == 2 ? 1 : 2) && count(TraceFactKind::history_committed) == (mode == 0 ? 2 : 1),
            "multi-step model or history facts missing");
        for (std::size_t i = 0; i < delta.facts.size(); ++i)
            check(delta.facts[i].revision == i+1 && delta.facts[i].subject.run == request->run_id(),"source revisions or run association corrupted");
    }
    check(request->trace_delta(delta.latest_revision).facts.empty() && request->trace_delta().latest_revision == delta.latest_revision && actions == 1,
        "trace query advanced or repeated execution");
}
}
// 用固定响应检验正式运行追踪，不触发真实服务。
int main() {
    try { for (unsigned mode = 0; mode < 4; ++mode) scenario(mode); std::cout << "Session journal checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
