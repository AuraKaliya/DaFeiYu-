#include <dafeiyu/run_trace.hpp>
#include <iostream>
#include <thread>

namespace {
using namespace dfy::agent;
using namespace dfy::runtime;
// 报告追踪投影与权威状态不一致。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 拒绝损坏元数据，避免伪造可读时间线。
void rejects(const auto& action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid trace accepted");
}
// 通过原宿主入口有界推进，不把诊断读取当作执行。
void drain(Session& session, const std::shared_ptr<TurnRequest>& request) {
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (request->exit()->state() == OperationState::pending && std::chrono::steady_clock::now() < deadline) { session.advance(); std::this_thread::yield(); }
    check(request->exit()->state() == OperationState::succeeded,"session did not exit");
}
// 验证真实单次路径投影、命名空间隔离，以及模型成功但历史提交失败的区别。
// 步骤：读取未结算快照 → 完成并保留值追踪 → 触发历史提交失败 → 拒绝损坏修订。
void session_trace() {
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    unsigned calls = 0;
    Session session({},channel,1,[&](const auto&,const auto& messages) { ++calls; return messages.back().content == "large" ? std::string(1024*1024,'x') : std::string("private answer"); });
    auto request = session.begin("private prompt");
    const auto live = single_run_trace(request->snapshot(),"fixture-boot");
    check(live.nodes.size() == 4 && live.nodes[0].result == OperationState::pending && live.nodes[3].result == OperationState::pending,
        "trace read advanced result");
    drain(session,request); const auto trace = single_run_trace(request->snapshot(),"fixture-boot");
    check(trace.events.size() == 4 && trace.nodes[3].parent == trace.nodes[2].id && trace.nodes[3].call_outcome == dfy::model::CallOutcome::returned &&
        trace.nodes[3].observed_exit_ns && calls == 1,"model trace association lost");
    check(single_run_trace(request->snapshot(),"another-boot").key != trace.key,"process namespaces merged");
    request.reset(); request = session.begin("large"); drain(session,request);
    const auto failed = single_run_trace(request->snapshot(),"fixture-boot");
    check(failed.nodes[0].result == OperationState::failed && failed.nodes[3].result == OperationState::succeeded &&
        failed.nodes[3].exit == OperationState::succeeded && trace.nodes[0].result == OperationState::succeeded && calls == 2,
        "history failure became a model failure or retained trace pinned a result");
    auto invalid = request->snapshot(); invalid.facts[0].revision = 2; rejects([&] { (void)single_run_trace(invalid,"fixture"); });
    rejects([&] { (void)single_run_trace(request->snapshot(),"../unsafe"); });
    invalid = request->snapshot(); invalid.diagnostics.accepted = TurnDiagnostics::TimePoint::min();
    invalid.diagnostics.model.submitted = TurnDiagnostics::TimePoint::max();
    rejects([&] { (void)single_run_trace(invalid,"fixture"); });
}
// 验证工具追踪不推进工作或持有实际代码，迟到状态不会改写旧值。
void tool_trace() {
    ToolDriver driver({AttemptId{StepId{RunId{4,5},2},1},8,{"call","fixture","{}"}},[](const auto&) {
        return completed_tool({false,ToolEffect::applied,"private output",ToolError::failed});
    });
    const auto before = tool_trace_node(*driver.request()); driver.advance(); const auto after = tool_trace_node(*driver.request());
    check(before.result == OperationState::pending && after.result == OperationState::failed && after.effect == ToolEffect::applied &&
        after.parent == TraceNodeId{{4,5},TraceNodeKind::attempt,2,1} && !after.call_started_ns,"tool trace changed fact or fabricated time");
}
}
// 以固定模型响应和工具结果验证值投影，不请求真实服务。
int main() {
    try { session_trace(); tool_trace(); std::cout << "Run trace checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
