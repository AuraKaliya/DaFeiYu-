#include <dafeiyu/subscription_reader.hpp>
#include <dafeiyu/completion_notifications.hpp>
#include <iostream>

namespace {
using namespace dfy::events;
using namespace dfy::runtime;
// 在读取取消、结果或延后通知违反契约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 向队列交付一个合法事件，归还生产者的来源引用。
void publish(const std::shared_ptr<EventStore>& store, SubscriptionQueue& queue) {
    auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("app.event", 1, {}, {0, 32}));
    auto value = std::make_shared<const StructValue>(StructValue::create(type, {}, 0));
    const auto event = store->accept(value);
    queue.push(event);
    store->release(event);
}

// 验证读取等待接入生产通知及 FIFO，消费者始终延后运行并能独立保留数据。
// 步骤：提交 Pending → 注册通知 → 接纳并推进数据 → 延后交付 → 回收请求及独立数据。
void verify_notification() {
    auto store = std::make_shared<EventStore>(4, 0);
    auto queue = std::make_shared<SubscriptionQueue>(store, 2);
    SubscriptionReader reader(queue, 2);
    CompletionNotifications notifications(1);
    FifoExecutor executor(1);
    auto request = reader.next();
    bool busy = false, pending = false, called = false;
    try { (void)reader.next(); } catch (const std::logic_error&) { busy = true; }
    try { (void)request->result(); } catch (const std::logic_error&) { pending = true; }
    EventLease retained;
    const auto registration = notifications.subscribe(request->operation(), [request, &called, &retained](const auto& operation) {
        check(operation->state() == OperationState::succeeded && request->result().outcome == ReadOutcome::event,
            "notification preceded read result");
        retained = request->retain_event();
        called = true;
    });
    check(busy && pending && notifications.schedule_one(executor) == ScheduleResult::idle, "pending read notified");
    publish(store, *queue);
    reader.advance();
    check(!called && notifications.schedule_one(executor) == ScheduleResult::queued && !called, "read completed inline callback");
    check(executor.run_one() == RunResult::executed && called, "read notification not delivered");
    notifications.release(registration);
    request.reset();
    reader.collect();
    check(!reader.size() && retained.get().id.sequence == 1 && store->references() == 1, "retained result lost");
    retained.reset();
    check(!store->references(), "read data leaked");
}

// 验证取消先到保留队列数据、已完成拒绝迟到取消、结果容量及关闭唤醒。
// 步骤：请求取消并推进 → 读取保留事件 → 检查终态及预算 → 关闭后读取结束。
void verify_cancellation_and_close() {
    auto store = std::make_shared<EventStore>(4, 0);
    auto queue = std::make_shared<SubscriptionQueue>(store, 2);
    SubscriptionReader reader(queue, 2);
    auto cancelled = reader.next();
    publish(store, *queue);
    cancelled->request_cancel(CancelReason::user);
    reader.advance();
    check(cancelled->result().outcome == ReadOutcome::cancelled && queue->size() == 1 && queue->state() == QueueState::open,
        "read cancellation consumed event or closed stream");
    auto event = reader.next();
    check(event->event().id.sequence == 1 && event->request_cancel(CancelReason::deadline) == CancelDisposition::already_settled,
        "late cancellation changed read");
    bool full = false;
    try { (void)reader.next(); } catch (const std::length_error&) { full = true; }
    check(full && reader.size() == 2, "retained results bypassed request budget");
    event.reset();
    cancelled.reset();
    auto ended = reader.next();
    reader.close(CloseMode::cancel);
    check(ended->operation()->state() == OperationState::succeeded && ended->result().outcome == ReadOutcome::closed &&
        ended->result().close_mode == CloseMode::cancel, "stream end confused with read cancellation");
}

// 验证客户端退出后宿主继续完成读取，控制器退出则明确取消未完成等待。
// 步骤：丢弃客户端持有 → 推进完成并回收 → 创建新等待 → 销毁控制器并检查取消原因。
void verify_host_ownership() {
    auto store = std::make_shared<EventStore>(2, 0);
    auto queue = std::make_shared<SubscriptionQueue>(store, 1);
    std::shared_ptr<ReadRequest> orphan;
    {
        SubscriptionReader reader(queue, 1);
        auto request = reader.next();
        const auto status = request->operation();
        request.reset();
        publish(store, *queue);
        reader.advance();
        reader.collect();
        check(status->state() == OperationState::succeeded && !reader.size() && !store->references(), "client release lost host work");
        orphan = reader.next();
    }
    check(orphan->result().outcome == ReadOutcome::cancelled && orphan->operation()->cancellation_reason() == CancelReason::owner_stop &&
        queue->state() == QueueState::open, "reader exit abandoned wait or cancelled stream");
}

// 验证排空关闭先交付已有事件，再单独返回带关闭原因的流结束。
// 步骤：等待并入队事件 → 排空关闭 → 读取最后事件 → 再次读取流结束。
void verify_draining_read() {
    auto store = std::make_shared<EventStore>(2, 0);
    auto queue = std::make_shared<SubscriptionQueue>(store, 1);
    SubscriptionReader reader(queue, 2);
    const auto event = reader.next();
    publish(store, *queue);
    reader.close(CloseMode::drain);
    check(event->result().outcome == ReadOutcome::event && !event->result().close_mode && event->event().id.sequence == 1,
        "draining event was confused with stream end");
    const auto ended = reader.next();
    check(ended->result().outcome == ReadOutcome::closed && ended->result().close_mode == CloseMode::drain,
        "draining read did not report end reason");
}
}

// 验收类型化可取消读取及延后通知，不提供 C ABI 装配或自动后台推进。
// 步骤：验证通知链路 → 验证取消与关闭 → 验证宿主持有责任 → 汇报结果。
int main() {
    try {
        verify_notification();
        verify_cancellation_and_close();
        verify_host_ownership();
        verify_draining_read();
        std::cout << "Subscription reader checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
