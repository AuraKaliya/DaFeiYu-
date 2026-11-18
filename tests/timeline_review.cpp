#include "support/scenario_replay.hpp"
#include <iostream>

namespace {
using namespace dfy::agent;
using namespace dfy::model;
using namespace dfy::tests;
// 报告只读诊断回顾没有诚实说明完整性或损坏。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 验证损坏诊断拒绝解释，不顺便执行任何工作。
void rejects(const auto& action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid timeline interpreted as valid");
}
// 取得由正式运行产生的纯值诊断，回顾时 Session 已经销毁。
RunTimeline timeline() {
    ScriptStep step; step.last_text = "goal"; step.response = Response{{Role::assistant,"answer"}};
    ReplayCase fixture; fixture.model = {1,"read_only_review",1,{},{step},1}; fixture.prompt = "goal";
    fixture.expected.history_messages = 2; return replay_scenario(fixture).diagnostic.timeline;
}
// 验证完整、分页、裁剪和投影故障的区别，不把诊断元数据当成可重跑材料。
void completeness() {
    const auto original = timeline(); const auto report = review_timeline(original);
    check(report.complete_history && report.run_exited && report.models == 1 && !report.replayable,"metadata pretended to be replay materials");
    auto partial = original; partial.delta.facts.resize(1); partial.delta.more = true;
    check(!review_timeline(partial).complete_history,"page pretended to be complete history");
    auto trimmed = original; trimmed.delta.facts.erase(trimmed.delta.facts.begin(),trimmed.delta.facts.end()-2);
    trimmed.delta.oldest_revision = trimmed.delta.facts.front().revision; trimmed.delta.gap = true;
    check(!review_timeline(trimmed).complete_history && review_timeline(trimmed).run_exited,"trim erased current run state");
    auto failed = original; failed.delta.projection_failed = true;
    failed.notifications.finished = dfy::events::ObservationReceipt{dfy::events::ObservationStatus::failed,{}};
    const auto issue = review_timeline(failed);
    check(!issue.complete_history && issue.run_exited && issue.notification_issue,"projection or notification fault altered execution outcome");
}
// 验证未知版本、重复或乱序事实及跨运行节点不覆盖原有效事实。
void corruption() {
    const auto original = timeline(); rejects([&] { review_timeline(original,99); });
    auto bad = original; bad.nodes.push_back(bad.nodes.back()); rejects([&] { review_timeline(bad); });
    bad = original; bad.nodes.back().parent.reset(); rejects([&] { review_timeline(bad); });
    bad = original; ++bad.delta.run.sequence; rejects([&] { review_timeline(bad); });
    bad = original; std::swap(bad.delta.facts[0],bad.delta.facts[1]); rejects([&] { review_timeline(bad); });
    bad = original; bad.delta.facts[1] = bad.delta.facts[0]; rejects([&] { review_timeline(bad); });
    bad = original; bad.delta.facts[0].subject.run.sequence = 99; rejects([&] { review_timeline(bad); });
    bad = original; bad.nodes[0].result = dfy::runtime::OperationState::pending; rejects([&] { review_timeline(bad); });
    bad = original; bad.delta.facts.back().result = dfy::runtime::OperationState::failed; rejects([&] { review_timeline(bad); });
    bad = original; bad.delta.facts[0].kind = TraceFactKind::model_exited; rejects([&] { review_timeline(bad); });
    bad = original; bad.delta.facts.clear(); bad.delta.latest_revision = 0; rejects([&] { review_timeline(bad); });
    bad = original; bad.nodes.back().call_started_ns = 10; bad.nodes.back().call_finished_ns = 9; rejects([&] { review_timeline(bad); });
    bad = original; bad.nodes[0].settled_ns = 100; bad.nodes[0].observed_exit_ns = 99; rejects([&] { review_timeline(bad); });
    bad = original; bad.model_contexts.clear(); rejects([&] { review_timeline(bad); });
    bad = original; ++bad.model_contexts[0].goal_revision; rejects([&] { review_timeline(bad); });
    bad = original; ++bad.model_contexts[0].attempt.step.run.sequence; rejects([&] { review_timeline(bad); });
    bad = original; bad.model_contexts[0].purpose = ModelPurpose::plan_step; rejects([&] { review_timeline(bad); });
    check(review_timeline(original).complete_history,"invalid copy mutated original facts");
}
}
// 诊断回顾只接收值，固定场景仅用于准备输入，不执行文件中的任何指令。
int main() {
    try { completeness(); corruption(); std::cout << "Timeline review checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
