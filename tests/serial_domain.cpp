#include <dafeiyu/serial_domain.hpp>
#include <dafeiyu/completion_notifications.hpp>
#include <iostream>
namespace {
using namespace dfy::runtime;
// 在 FIFO 授予、占用或关闭责任违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 核对指定拒绝类型，保留无关异常供入口报告。
template<class Error, class Action>
void rejects(Action action) { try { action(); } catch (const Error&) { return; } throw std::runtime_error("serial domain accepted invalid action"); }
// 验证显式授予、等待不释放占用、取消跳过及独立域推进。
// 步骤：有界排队 → 授予首项并取消等待 → 验证其他域及错误完成 → 归还后按序推进。
void ordering() {
    SerialDomain domain(3), other(1);
    auto first = domain.submit(), second = domain.submit(), third = domain.submit();
    check(domain.pending() == 3 && !domain.active() && first->admission()->state() == OperationState::pending &&
        first->sequence() == 1 && third->sequence() == 3 && first->domain() == domain.id(),"submission granted inline or lost identity");
    rejects<std::length_error>([&] { (void)domain.submit(); });
    domain.advance();
    check(domain.active() == first && first->phase() == SerialPhase::granted && first->admission()->state() == OperationState::succeeded &&
        second->admission()->state() == OperationState::pending,"first serial grant wrong");
    check(first->request_cancel(CancelReason::deadline) == CancelDisposition::already_settled,"late admission cancel ended active work");
    second->request_cancel(CancelReason::user); domain.advance();
    check(second->phase() == SerialPhase::cancelled && second->admission()->state() == OperationState::cancelled && domain.active() == first &&
        third->phase() == SerialPhase::waiting,"cancelled waiter released active domain");
    const auto unrelated = other.submit(); other.advance();
    check(other.id() != domain.id() && other.active() == unrelated,"independent domain blocked");
    rejects<std::invalid_argument>([&] { domain.finish(unrelated); });
    rejects<std::invalid_argument>([&] { domain.finish({}); });
    rejects<std::logic_error>([&] { domain.finish(third); });
    auto snapshot = domain.active(); first.reset(); domain.collect(); domain.advance();
    check(domain.active() == snapshot && third->phase() == SerialPhase::waiting,"client release or waiting yielded serial occupancy");
    domain.finish(snapshot);
    check(snapshot->phase() == SerialPhase::released && snapshot->admission()->state() == OperationState::succeeded && !domain.active() &&
        third->phase() == SerialPhase::waiting,"finish rewrote grant or granted next inline");
    rejects<std::logic_error>([&] { domain.finish(snapshot); });
    snapshot.reset(); domain.collect();
    const auto fourth = domain.submit();
    check(fourth->sequence() == 4,"capacity rejection consumed sequence");
    domain.advance(); check(domain.active() == third,"slot reuse changed FIFO order");
    domain.finish(third); domain.advance(); check(domain.active() == fourth,"next serial request lost");
    domain.finish(fourth); other.finish(unrelated);
    check(!domain.active() && !domain.pending() && second->admission()->cancellation_reason() == CancelReason::user,"serial release or first cancel reason changed");
}
// 验证关闭只终结等待，保留终态不阻止逻辑关闭但继续占记录预算。
// 步骤：活动期间排空并升级取消 → 归还活动占用 → 验证终态预算 → 验证完整排空顺序。
void closure() {
    SerialDomain domain(3);
    auto first = domain.submit(), second = domain.submit(), third = domain.submit(); domain.advance();
    rejects<std::invalid_argument>([&] { domain.close(static_cast<SerialClose>(99)); });
    check(!domain.close_mode(),"invalid close changed admission");
    domain.close(SerialClose::drain); rejects<std::logic_error>([&] { (void)domain.submit(); });
    second->request_cancel(CancelReason::deadline); domain.close(SerialClose::cancel); domain.close(SerialClose::drain);
    check(domain.active() == first && !domain.closed() && second->phase() == SerialPhase::cancelled && third->phase() == SerialPhase::cancelled &&
        second->admission()->cancellation_reason() == CancelReason::deadline && third->admission()->cancellation_reason() == CancelReason::owner_stop &&
        domain.close_mode() == SerialClose::cancel,"cancel close ended active work or overwrote first reason");
    domain.finish(first); domain.collect();
    check(domain.closed() && domain.size() == 3,"closed domain dropped externally retained snapshots");
    first.reset(); second.reset(); third.reset(); domain.collect(); check(!domain.size(),"serial terminal snapshots leaked");

    SerialDomain draining(2); const auto a = draining.submit(), b = draining.submit(); draining.close(SerialClose::drain);
    check(!draining.closed() && !draining.active(),"drain granted work inline or discarded queue");
    draining.advance(); check(draining.active() == a,"drain did not grant first work");
    draining.finish(a); draining.advance(); check(draining.active() == b,"drain did not preserve order");
    draining.finish(b); draining.close(SerialClose::cancel);
    check(draining.closed() && draining.close_mode() == SerialClose::drain,"closed drain reason was rewritten");
}
// 验证控制器退出不伪造处理完成，客户端释放也不丢失宿主授予责任。
// 步骤：退出时取消等待 → 保留活动事实 → 无客户端授予并显式归还 → 拒绝零容量。
void lifetime() {
    std::shared_ptr<SerialRequest> active, waiting;
    {
        SerialDomain domain(2); active = domain.submit(); waiting = domain.submit(); domain.advance();
    }
    check(active->phase() == SerialPhase::granted && active->admission()->state() == OperationState::succeeded &&
        waiting->phase() == SerialPhase::cancelled && waiting->admission()->cancellation_reason() == CancelReason::owner_stop,
        "domain exit falsely completed granted work or lost waiting cancellation");
    SerialDomain orphan(1); auto client = orphan.submit(); const auto admission = client->admission(); client.reset(); orphan.collect();
    check(orphan.size() == 1 && admission->state() == OperationState::pending,"client release cancelled accepted wait");
    orphan.advance(); check(orphan.active() && admission->state() == OperationState::succeeded,"host lost unreferenced grant");
    orphan.finish(orphan.active()); orphan.collect(); check(!orphan.size(),"host could not release unreferenced occupancy");
    rejects<std::invalid_argument>([] { SerialDomain invalid(0); });
}
// 验证延后授予通知、跨域推进和异步恢复，原域直到宿主确认处理结束才放行。
// 步骤：登记授予与恢复通知 → 启动首项后等待 → 执行其他域 → 恢复原处理并放行后续。
void continuation() {
    SerialDomain ordered(2), independent(1);
    const auto first = ordered.submit(), next = ordered.submit(), other = independent.submit();
    OperationSource completion;
    CompletionNotifications notices(4); FifoExecutor executor(1);
    std::vector<int> trace;
    const auto first_notice = notices.subscribe(first->admission(),[&](const auto&) { trace.push_back(1); });
    const auto next_notice = notices.subscribe(next->admission(),[&](const auto&) { trace.push_back(4); ordered.finish(next); });
    const auto other_notice = notices.subscribe(other->admission(),[&](const auto&) { trace.push_back(2); independent.finish(other); });
    const auto resumed = notices.subscribe(completion.operation(),[&](const auto&) { trace.push_back(3); ordered.finish(first); });
    ordered.advance(); independent.advance();
    check(trace.empty() && notices.schedule_one(executor) == ScheduleResult::queued && notices.schedule_one(executor) == ScheduleResult::blocked,
        "serial admission called work inline or ignored FIFO backpressure");
    check(executor.run_one() == RunResult::executed && trace == std::vector<int>{1},"first ordered work did not start");
    ordered.advance();
    check(ordered.active() == first && next->admission()->state() == OperationState::pending,"asynchronous waiting released ordering occupancy");
    check(notices.schedule_one(executor) == ScheduleResult::queued && executor.run_one() == RunResult::executed && trace == std::vector<int>({1,2}),
        "waiting domain blocked independent work");
    check(notices.schedule_one(executor) == ScheduleResult::idle,"later ordered work ran before original completion");
    completion.settle(OperationState::succeeded);
    check(trace == std::vector<int>({1,2}) && ordered.active() == first,"completion signal executed continuation inline");
    check(notices.schedule_one(executor) == ScheduleResult::queued && executor.run_one() == RunResult::executed && trace == std::vector<int>({1,2,3}) &&
        next->admission()->state() == OperationState::pending,"resume repeated grant or started next inline");
    ordered.advance();
    check(notices.schedule_one(executor) == ScheduleResult::queued && executor.run_one() == RunResult::executed && trace == std::vector<int>({1,2,3,4}) &&
        !ordered.active(),"ordered continuation lost FIFO progression");
    for (const auto notice : {first_notice,next_notice,other_notice,resumed}) notices.release(notice);
    notices.collect(); check(!notices.size() && !notices.references() && executor.run_one() == RunResult::idle,"serial notification chain leaked");
}
// 验证通知撤销或回调失败不被误认为处理结束，宿主必须明确归还已经授予的占用。
// 步骤：授予后撤销入口 → 核对仍占用 → 执行失败回调 → 显式确认结束后归还。
void delivery_failures() {
    SerialDomain domain(2); auto first = domain.submit(), next = domain.submit();
    CompletionNotifications notices(2); FifoExecutor executor(1); unsigned calls = 0;
    const auto revoked = notices.subscribe(first->admission(),[&](const auto&) { ++calls; });
    domain.advance(); check(notices.schedule_one(executor) == ScheduleResult::queued && notices.revoke(revoked) == RevokeResult::revoked,"serial revoke setup failed");
    check(executor.run_one() == RunResult::executed && !calls && domain.active() == first && next->phase() == SerialPhase::waiting,
        "revoking grant notification silently released occupancy");
    notices.release(revoked); domain.finish(first); domain.advance();
    const auto failed = notices.subscribe(next->admission(),[&](const auto&) { ++calls; throw std::runtime_error("simulated work failure"); });
    check(notices.schedule_one(executor) == ScheduleResult::queued && executor.run_one() == RunResult::failed && calls == 1 && domain.active() == next,
        "callback failure pretended to confirm processing completion");
    domain.finish(next); notices.release(failed); notices.collect();
    check(!domain.active() && !notices.size() && executor.run_one() == RunResult::idle,"failed serial notification leaked");
}
}
// 验收顺序域的授予、占用和结束边界，授予成功不冒充业务结果。
// 步骤：核对顺序与关闭 → 验证责任寿命 → 联测通知恢复及失败 → 报告结果。
int main() {
    try { ordering(); closure(); lifetime(); continuation(); delivery_failures(); std::cout << "Serial domain checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
