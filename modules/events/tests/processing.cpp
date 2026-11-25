#include <dafeiyu/processing.hpp>
#include <dafeiyu/processing_queue.hpp>
#include <iostream>
#include <cstdlib>
#include <cstdio>
namespace {
using namespace dfy::runtime;
using namespace dfy::events;
// 在业务结果、退出和顺序占用边界违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 验证错误类别并保留无关失败供入口报告。
template<class Error, class Action>
void rejects(Action action) { try { action(); } catch (const Error&) { return; } throw std::runtime_error("processing accepted invalid action"); }
// 接纳一项真实事件并转交数据持有，不依赖替身处理记录。
EventLease event(const std::shared_ptr<EventStore>& store) {
    const auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.processing",1,{}, {0,32}));
    const auto handle = store->accept(std::make_shared<const StructValue>(StructValue::create(type,{},0)));
    SubscriptionQueue queue(store,1); queue.push(handle); store->release(handle);
    return queue.poll().event;
}
// 验证业务成功仍占用顺序域，实际退出后数据和终态可独立保留。
// 步骤：绑定两项真实交付 → 按序开始 → 先完成业务再确认退出 → 保留数据并归还处理资源。
void lifecycle() {
    auto store = std::make_shared<EventStore>(12,0); auto data = event(store);
    auto domain = std::make_shared<SerialDomain>(2);
    const auto first = domain->submit(), second = domain->submit();
    auto a = std::make_unique<ProcessingSource>(data,domain,first);
    ProcessingSource b(data,domain,second); const auto record = a->record();
    rejects<std::logic_error>([&] { ProcessingSource duplicate(data,domain,first); });
    check(a->start() == StartResult::waiting && b.start() == StartResult::waiting,"processing granted its own order");
    domain->advance(); check(a->start() == StartResult::started,"granted processing did not start");
    rejects<std::logic_error>([&] { a->finish(); });
    a->complete(OperationState::succeeded); domain->advance();
    check(record->completion()->state() == OperationState::succeeded && record->exit()->state() == OperationState::pending &&
        domain->active() == first && b.start() == StartResult::waiting,"business result released actual processing");
    check(record->request_cancel(CancelReason::owner_stop) == CancelDisposition::recorded,"result prevented cleanup cancellation");
    rejects<std::logic_error>([&] { a->complete(OperationState::failed); });
    auto retained = record->retain_event(); a->finish(); a.reset(); data.reset();
    check(record->phase() == ProcessingPhase::exited && record->event().id == retained.get().id && !domain->active(),"exit invalidated data or kept order");
    domain->advance(); check(b.start() == StartResult::started,"next processing did not start after actual exit");
    b.record()->request_cancel(CancelReason::user);
    check(b.record()->completion()->state() == OperationState::pending && domain->active() == second,"running cancel forged completion");
    b.complete(OperationState::cancelled); b.finish();
}
// 验证开始前撤回、域关闭及准备失败均不会消耗原事件或伪造业务成功。
// 步骤：取消等待及授予 → 检查独立处理 → 验证参数和引用预算 → 验证完成源退出。
void cancellation() {
    auto store = std::make_shared<EventStore>(8,0); auto data = event(store);
    for (const bool granted : {false,true}) {
        auto domain = std::make_shared<SerialDomain>(1); const auto order = domain->submit();
        ProcessingSource source(data,domain,order);
        if (granted) domain->advance();
        source.record()->request_cancel(CancelReason::deadline);
        check(source.start() == StartResult::exited && !domain->active() && !domain->pending() &&
            source.record()->completion()->state() == OperationState::cancelled,"unstarted cancellation left occupancy");
    }
    auto domain = std::make_shared<SerialDomain>(1); const auto order = domain->submit();
    rejects<std::invalid_argument>([&] { ProcessingSource invalid(data,domain); });
    rejects<std::invalid_argument>([&] { ProcessingSource invalid(data,{},order); });
    ProcessingSource closed(data,domain,order); domain->close(SerialClose::cancel);
    check(closed.start() == StartResult::exited,"closed ordering did not cancel processing");
    std::shared_ptr<ProcessingRecord> snapshot;
    { ProcessingSource unused(data); snapshot = unused.record(); }
    check(snapshot->phase() == ProcessingPhase::exited && snapshot->exit()->cancellation_reason() == CancelReason::owner_stop,
        "unstarted source destruction left pending result");
    auto tiny = std::make_shared<EventStore>(2,0); auto full = event(tiny); auto extra = full.retain();
    auto retry_domain = std::make_shared<SerialDomain>(1); auto retry = retry_domain->submit();
    rejects<ReferenceError>([&] { ProcessingSource rejected(full,retry_domain,retry); });
    extra.reset(); ProcessingSource accepted(full,retry_domain,retry);
    check(full.get().id == accepted.record()->event().id,"failed preparation consumed data or binding");
}
// 验证队列独立持有在途责任，接受时定序，取消后等待真正退出。
// 步骤：接纳并释放客户端 → 按固定顺序开始 → 关闭并保留运行项 → 退出后回收预算。
void queue_lifetime() {
    auto store = std::make_shared<EventStore>(10,0); auto data = event(store);
    ProcessingQueue queue(2), other(1); auto first = queue.submit(data), second = queue.submit(data);
    rejects<std::length_error>([&] { (void)queue.submit(data); });
    rejects<std::invalid_argument>([&] { (void)other.start(first); });
    const auto completion = first->completion(); first.reset(); queue.advance();
    auto active = queue.ready();
    check(active && active->ordering()->sequence() == 1 && queue.start(active) == StartResult::started,"client release lost ordered work");
    queue.complete(active,OperationState::succeeded); queue.advance();
    check(!queue.ready() && completion->state() == OperationState::succeeded,"completed business admitted next before exit");
    queue.close(CloseMode::cancel);
    check(!queue.closed() && second->completion()->state() == OperationState::cancelled &&
        active->exit()->cancellation_reason() == CancelReason::owner_stop,"queue close forged running exit");
    queue.finish(active); check(queue.closed() && queue.size() == 2,"queue close discarded terminal snapshots");
    active.reset(); second.reset(); queue.collect(); check(!queue.size(),"processing queue leaked terminal capacity");
    rejects<std::logic_error>([&] { (void)queue.submit(data); });

    ProcessingQueue draining(2); auto a = draining.submit(data), b = draining.submit(data);
    draining.close(CloseMode::drain); draining.advance(); draining.start(a);
    draining.complete(a,OperationState::failed); draining.finish(a); a.reset(); draining.advance();
    check(draining.ready() == b && b->ordering()->sequence() == 2,"drain or slot reuse changed accepted order");
    draining.start(b); draining.complete(b,OperationState::succeeded); draining.finish(b);
    check(draining.closed(),"drain did not finish after actual processing exit");

    ProcessingQueue parallel(2,false); const auto x = parallel.submit(data), y = parallel.submit(data);
    check(!x->ordering() && parallel.start(y) == StartResult::started && parallel.start(x) == StartResult::started,"parallel queue added serial constraint");
    parallel.close(CloseMode::cancel);
    for (const auto& item : {x,y}) { parallel.complete(item,OperationState::cancelled); parallel.finish(item); }
    check(parallel.closed(),"parallel exit did not close queue");
}
// 验证域控制器撤回已经授予但尚未开始的请求后，完成源能够观察取消并安全退出。
// 步骤：授予并外部撤回 → 通过推进或析构结束处理 → 核对首因及唯一取消结果。
void external_withdrawal() {
    const auto previous = std::set_terminate([]() noexcept { std::fputs("withdrawn processing source terminated during cleanup\n",stderr); std::_Exit(2); });
    auto store = std::make_shared<EventStore>(8,0); auto data = event(store);
    for (const bool advance : {true,false}) {
        auto domain = std::make_shared<SerialDomain>(1); const auto order = domain->submit(); std::shared_ptr<ProcessingRecord> result;
        {
            ProcessingSource source(data,domain,order); result = source.record(); domain->advance();
            domain->withdraw(order,CancelReason::deadline);
            if (advance) check(source.start() == StartResult::exited,"withdrawn grant was treated as executable");
        }
        check(result->completion()->state() == OperationState::cancelled && result->exit()->state() == OperationState::succeeded &&
            result->exit()->cancellation_reason() == CancelReason::deadline && !domain->active(),"external withdrawal lost cancellation or exit");
    }
    std::set_terminate(previous);
}
}
// 验收真实事件处理的业务完成、协作取消、实际退出及数据寿命。
// 步骤：验证顺序生命周期 → 验证取消与准备失败 → 报告结果。
int main() {
    try { lifecycle(); cancellation(); queue_lifetime(); external_withdrawal(); std::cout << "Processing checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
