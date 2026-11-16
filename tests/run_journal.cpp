#include <dafeiyu/run_journal.hpp>
#include <iostream>

namespace {
using namespace dfy::agent;
// 报告修订、缺口或查询隔离不满足契约。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 验证非法游标或容量明确拒绝，不返回看似完整的时间线。
void rejects(const auto& action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid journal query accepted");
}
// 验证有限保留、分页和重复查询保持原来源修订与顺序。
void windows() {
    RunJournal journal({1,1},3); TraceFact fact; fact.subject = {{1,1}};
    check(journal.read().facts.empty() && !journal.read().gap,"empty journal fabricated a gap");
    for (unsigned i = 0; i < 6; ++i) { fact.source_revision = 6-i; journal.append(fact); }
    const auto first = journal.read(0,1);
    check(first.gap && first.more && first.oldest_revision == 4 && first.latest_revision == 6 && first.facts[0].revision == 4,
        "trimmed journal pretended to be complete");
    const auto next = journal.read(4,2);
    check(!next.gap && !next.more && next.facts.size() == 2 && next.facts[0].revision == 5 && next.facts[1].revision == 6 &&
        next.facts[0].source_revision == 2 && journal.read(4,2).facts[0].observed == next.facts[0].observed,
        "query consumed, reordered or replaced source facts");
    check(journal.read(6).facts.empty() && journal.revision() == 6,"up-to-date query changed journal");
    rejects([&] { journal.read(7); }); rejects([&] { journal.read(0,0); }); rejects([&] { RunJournal invalid({1,1},1025); });
}
// 验证投影输入损坏只显示故障，不发布伪造节点或跨运行关联。
void failed_projection() {
    RunJournal journal({1,1}); TraceFact value; value.subject = {{1,2}}; journal.append(value);
    check(journal.read().projection_failed && !journal.revision(),"foreign projection was published or fault hidden");
    value.subject = {{1,1},TraceNodeKind::model,1,1,3}; value.kind = TraceFactKind::model_exited; journal.append(value);
    const auto result = journal.read();
    check(result.projection_failed && result.facts.size() == 1 && result.facts[0].revision == 1,"projection failure destroyed later valid facts");
}
}
// 只验证内存追踪查询，不构造模型或工具。
int main() {
    try { windows(); failed_projection(); std::cout << "Run journal checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
