#include <dafeiyu/serial_wait_graph.hpp>
#include <iostream>
namespace {
using namespace dfy::runtime;
// 在等待图环检查、身份或预算违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 验证明确拒绝类别，保留无关异常供入口报告。
template<class Error, class Action>
void rejects(Action action) { try { action(); } catch (const Error&) { return; } throw std::runtime_error("serial wait accepted invalid dependency"); }
// 验证同域等待和跨域环，重复登记独立归还，旧处理结束不误伤后续同域工作。
// 步骤：建立三个活动域 → 检查自等待与闭环 → 独立释放重复依赖 → 结束旧处理并验证新轮次。
void cycles() {
    SerialDomain a(2), b(2), c(1);
    const auto first = a.submit(), queued = a.submit(), second = b.submit(), third = c.submit();
    a.advance(); b.advance(); c.advance();
    SerialWaitGraph graph(4);
    rejects<std::logic_error>([&] { (void)graph.add(first,first); });
    rejects<std::logic_error>([&] { (void)graph.add(first,queued); });
    rejects<std::logic_error>([&] { (void)graph.add(queued,second); });
    const auto ab = graph.add(first,second), bc = graph.add(second,third);
    rejects<std::logic_error>([&] { (void)graph.add(third,first); });
    check(graph.size() == 2 && graph.active(ab) && graph.active(bc) && a.active() == first && c.active() == third,"cycle rejection changed processing");
    const auto duplicate = graph.add(first,second); graph.release(ab);
    rejects<std::logic_error>([&] { (void)graph.add(second,first); });
    graph.release(duplicate);
    const auto ba = graph.add(second,first);
    check(graph.active(ba),"releasing all duplicate waits did not remove edge");
    a.finish(first); a.advance();
    check(!graph.active(ba) && a.active() == queued,"old dependency remained active after owner target released");
    const auto next = graph.add(queued,second);
    rejects<std::logic_error>([&] { (void)graph.add(third,queued); });
    check(graph.active(next),"new request in same domain was blocked by stale dependency");
    c.finish(third);
    check(!graph.active(bc) && graph.add(queued,third).empty(),"finished target required an unnecessary wait");
    graph.release({}); check(!graph.active({}),"empty dependency was active");
    for (const auto handle : {bc,ba,next}) graph.release(handle);
    rejects<ReferenceError>([&] { graph.release(next); });
    check(!graph.size(),"wait registrations leaked");
    a.finish(queued); b.finish(second);
}
// 验证登记容量和跨图身份，图只弱引用请求，不延长终态预算。
// 步骤：填满登记并拒绝扩容 → 完成目标但保留登记 → 归还并复用 → 释放请求后验证弱引用。
void ownership() {
    SerialDomain a(1), b(2); const auto owner = a.submit(); auto target = b.submit();
    a.advance(); b.advance(); SerialWaitGraph graph(1), other(1);
    const auto edge = graph.add(owner,target);
    rejects<ReferenceError>([&] { (void)graph.add(owner,target); });
    rejects<ReferenceError>([&] { (void)other.active(edge); });
    rejects<ReferenceError>([&] { other.release(edge); });
    b.finish(target); target.reset(); b.collect();
    check(!b.size() && !graph.active(edge) && graph.size() == 1,"wait graph kept terminal request or discarded owner registration");
    auto replacement = b.submit(); b.advance();
    rejects<ReferenceError>([&] { (void)graph.add(owner,replacement); });
    graph.release(edge);
    const auto reused = graph.add(owner,replacement);
    check(reused != edge && graph.active(reused),"wait graph reused released identity");
    rejects<ReferenceError>([&] { (void)graph.active(edge); });
    rejects<std::invalid_argument>([&] { (void)graph.add({},replacement); });
    const auto cancelled = b.submit(); cancelled->request_cancel(CancelReason::user); b.advance();
    check(graph.add(owner,cancelled).empty(),"cancelled target consumed a full graph slot");
    a.finish(owner); check(!graph.active(reused),"released owner left an active wait constraint");
    graph.release(reused); b.finish(replacement);
    rejects<std::invalid_argument>([] { SerialWaitGraph invalid(0); });
}
}
// 验收显式等待图，只阻止已声明的顺序域循环，不推断未声明的外部依赖。
// 步骤：核对等待环 → 核对容量及弱引用寿命 → 报告结果。
int main() {
    try { cycles(); ownership(); std::cout << "Serial wait graph checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
