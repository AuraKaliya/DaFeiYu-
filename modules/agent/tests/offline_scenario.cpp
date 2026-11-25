#include "support/scripted_model.hpp"
#include <dafeiyu/run_trace.hpp>
#include <iostream>
#include <thread>

namespace {
using namespace dfy::tests;
using namespace dfy::agent;
using namespace dfy::model;
using namespace dfy::runtime;
// 报告功能场景结果与预期不一致。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 断言损坏场景或不满足的脚本契约被拒绝。
void rejects(const auto& action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid scenario accepted");
}
// 构造只有固定回答的版本化基础场景。
ScenarioSpec plain() {
    return {1,"plain_question",1,{},{{Role::user,std::string("hello"),{}, {},Response{{Role::assistant,"answer"}},false,std::chrono::milliseconds(5)}},1};
}
// 通过正式 Session 验证固定响应与轨迹，不因超时或缺少凭据连接真实服务。
// 步骤：构造显式后端 → 接纳一次真实会话 → 推进到实际退出 → 核对结果、版本和脚本消耗。
void session_case(bool failure) {
    auto spec = plain();
    if (failure) { spec.id = "model_failure"; spec.steps[0].response.reset(); spec.steps[0].fail = true; }
    ScriptedModel model(std::move(spec));
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    Session session({},channel,model.backend()); const auto request = session.begin("hello");
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (request->exit()->state() == OperationState::pending && std::chrono::steady_clock::now() < deadline) { session.advance(); std::this_thread::yield(); }
    check(request->exit()->state() == OperationState::succeeded && request->operation()->state() == (failure ? OperationState::failed : OperationState::succeeded),"scenario run outcome mismatch");
    model.verify_complete(); const auto report = model.report(); const auto trace = single_run_trace(request->snapshot(),report.case_id);
    check(report.attempted == 1 && report.consumed == 1 && !report.violations && report.case_version == 1 &&
        report.virtual_elapsed_ms == 5 && trace.key.run == request->run_id(),"scenario identity, calls or trace mismatch");
    check(model.clock()() == ToolTime{}+std::chrono::milliseconds(5),"virtual clock lost declared advance");
    model.advance_clock(std::chrono::milliseconds(3)); check(model.report().virtual_elapsed_ms == 8,"external clock event lost");
}
// 验证空或不受支持的场景、未知夹具、失配请求和额外调用均显式失败。
void refusals() {
    auto spec = plain(); spec.steps.clear(); rejects([&] { ScriptedModel missing(spec); });
    spec = plain(); spec.schema_version = 2; rejects([&] { ScriptedModel future(spec); });
    spec = plain(); spec.fixtures = {"external.command"}; rejects([&] { ScriptedModel unbound(spec,{"fixture.counter"}); });
    spec = plain(); spec.steps[0].response.reset(); rejects([&] { ScriptedModel missing(spec); });
    spec = plain(); spec.expected_model_requests.reset(); rejects([&] { ScriptedModel unspecified(spec); });
    spec = plain(); spec.steps[0].tool_names = {"one","one"}; rejects([&] { ScriptedModel duplicates(spec); });
    spec = plain(); spec.steps.clear(); spec.expected_model_requests = 0;
    ScriptedModel zero(spec); zero.verify_complete();
    rejects([&] { (void)zero.backend().complete({},{{{Role::user,"unexpected"}}}); });
    check(zero.report().attempted == 1 && zero.report().violations == 1,"zero-call fixture silently fell back");
    ScriptedModel mismatch(plain()); const auto backend = mismatch.backend();
    rejects([&] { (void)backend.complete({},{{{Role::user,"other"}}}); });
    rejects([&] { mismatch.verify_complete(); });
    ScriptedModel extra(plain()); const auto fixed = extra.backend();
    (void)fixed.complete({},{{{Role::user,"hello"}}});
    rejects([&] { (void)fixed.complete({},{{{Role::user,"hello"}}}); });
    rejects([&] { extra.verify_complete(); });
    check(extra.report().attempted == 2 && extra.report().violations == 1,"unexpected request hidden from report");
}
}
// 运行固定功能场景，不以通过率评价真实模型决策质量。
int main() {
    try { session_case(false); session_case(true); refusals(); std::cout << "Offline scenario checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
