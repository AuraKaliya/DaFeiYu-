#include <dafeiyu/completion_notifications.hpp>
#include <iostream>

namespace {
using namespace dfy::runtime;
// 在登记或引用寿命违反契约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 验证引用拒绝类别，不把容量失败与过期句柄混淆。
// 步骤：执行引用操作 → 核对拒绝类别 → 报告未拒绝的调用。
template<class Action>
void rejects_reference(ReferenceFailure expected, Action action) {
    try { action(); } catch (const ReferenceError& error) { check(error.reason() == expected, "wrong reference failure"); return; }
    throw std::runtime_error("notification reference unexpectedly accepted");
}

// 验证独立撤销、引用不可共享、容量拒绝及旧引用失效。
// 步骤：建立两项登记 → 检查共享与容量 → 撤销其中一项 → 复用槽位并核对其余登记。
void verify_registration() {
    CompletionNotifications notifications(2);
    OperationSource source(true);
    bool called = false;
    const auto callback = [&](const auto&) { called = true; };
    const auto first = notifications.subscribe(source.operation(), callback);
    const auto second = notifications.subscribe(source.operation(), callback);
    check(!called && notifications.size() == 2 && notifications.references() == 2, "registration invoked callback");
    rejects_reference(ReferenceFailure::not_shareable, [&] { (void)notifications.retain(first); });
    rejects_reference(ReferenceFailure::exhausted, [&] { (void)notifications.subscribe(source.operation(), callback); });
    check(notifications.revoke(first) == RevokeResult::revoked && notifications.revoke(first) == RevokeResult::already_inactive,
        "revoke not stable");
    check(notifications.read(first) == NotificationState::revoked && notifications.read(second) == NotificationState::pending &&
        source.operation()->cancellation_reason() == CancelReason::none, "revoke affected independent work");
    notifications.release(first);
    const auto reused = notifications.subscribe(source.operation(), callback);
    check(reused != first && notifications.size() == 2, "registration identity reused");
    rejects_reference(ReferenceFailure::invalid_handle, [&] { (void)notifications.read(first); });
    notifications.revoke(reused);
    notifications.release(reused);
    notifications.revoke(second);
    notifications.release(second);
    rejects_reference(ReferenceFailure::invalid_handle, [&] { notifications.release(second); });
    source.settle(OperationState::succeeded);
    check(!notifications.size() && !notifications.references() && !called, "terminal registration leaked");
}

// 验证操作及捕获被独立保持，释放客户端引用不隐式撤销。
// 步骤：从已完成操作登记 → 退出来源作用域 → 撤销并确认资源释放 → 验证无客户端的待通知责任。
void verify_ownership() {
    CompletionNotifications notifications(1);
    std::weak_ptr<const Operation> operation_lifetime;
    auto capture = std::make_shared<int>(1);
    const std::weak_ptr<int> capture_lifetime = capture;
    Handle registration;
    {
        OperationSource source;
        source.settle(OperationState::succeeded);
        operation_lifetime = source.operation();
        registration = notifications.subscribe(source.operation(), [capture](const auto&) {});
    }
    capture.reset();
    check(!capture_lifetime.expired() && !operation_lifetime.expired(), "registration borrowed lifetime");
    notifications.revoke(registration);
    check(capture_lifetime.expired() && operation_lifetime.expired() && notifications.size() == 1,
        "revoke retained callback or lost client snapshot");
    notifications.release(registration);
    {
        CompletionNotifications pending(1);
        OperationSource source;
        source.settle(OperationState::succeeded);
        operation_lifetime = source.operation();
        const auto released = pending.subscribe(source.operation(), [](const auto&) {});
        pending.release(released);
        check(pending.size() == 1 && !pending.references(), "release implicitly revoked notification");
        rejects_reference(ReferenceFailure::exhausted, [&] { (void)pending.subscribe(source.operation(), [](const auto&) {}); });
    }
    check(operation_lifetime.expired(), "pool destruction leaked operation");
    bool invalid = false;
    try { (void)notifications.subscribe({}, [](const auto&) {}); } catch (const std::invalid_argument&) { invalid = true; }
    check(invalid && !notifications.size() && !notifications.references(), "invalid registration leaked");
    OperationSource source;
    invalid = false;
    try { (void)notifications.subscribe(source.operation(), {}); } catch (const std::invalid_argument&) { invalid = true; }
    check(invalid && !notifications.size(), "empty callback accepted");
    source.settle(OperationState::failed);
}

// 验证槽位复用后的登记顺序、活动自撤销、回调保留和新登记的延后交付。
// 步骤：复用早期空槽 → 排队旧登记 → 在回调内登记后继 → 逐次推进并验证一次交付。
void verify_delivery_order() {
    CompletionNotifications notifications(3);
    FifoExecutor executor(2);
    OperationSource source;
    std::vector<int> trace;
    const auto discarded = notifications.subscribe(source.operation(), [](const auto&) {});
    Handle first, third;
    std::shared_ptr<const Operation> retained;
    // 在通知内检查活动状态并追加新通知，下一次宿主推进之前不得执行后继。
    // 步骤：核对终态与自撤销 → 保留操作 → 登记并排队后继。
    first = notifications.subscribe(source.operation(), [&](const auto& operation) {
        check(operation->state() == OperationState::succeeded && notifications.read(first) == NotificationState::running &&
            notifications.revoke(first) == RevokeResult::busy, "active notification not protected");
        retained = operation;
        trace.push_back(1);
        third = notifications.subscribe(operation, [&](const auto&) { trace.push_back(3); });
        check(notifications.schedule_one(executor) == ScheduleResult::queued, "callback registration not queued");
    });
    notifications.revoke(discarded);
    notifications.release(discarded);
    const auto second = notifications.subscribe(source.operation(), [&](const auto&) { trace.push_back(2); });
    check(notifications.schedule_one(executor) == ScheduleResult::idle, "pending operation scheduled");
    source.settle(OperationState::succeeded);
    check(notifications.schedule_one(executor) == ScheduleResult::queued &&
        notifications.schedule_one(executor) == ScheduleResult::queued &&
        notifications.schedule_one(executor) == ScheduleResult::idle && trace.empty(), "schedule reentered or duplicated");
    check(executor.run_one() == RunResult::executed && trace == std::vector<int>{1}, "slot reuse changed delivery order");
    check(executor.run_one() == RunResult::executed && executor.run_one() == RunResult::executed &&
        trace == std::vector<int>({1, 2, 3}), "callback registration ran early");
    for (const auto handle : {first, second, third}) {
        check(notifications.read(handle) == NotificationState::delivered &&
            notifications.revoke(handle) == RevokeResult::already_inactive, "delivery not terminal");
        notifications.release(handle);
    }
    check(!notifications.size() && retained->state() == OperationState::succeeded && executor.run_one() == RunResult::idle,
        "delivered entry or retained operation lost");
}

// 验证执行器背压、排队后撤销及客户端引用释放后的交付责任。
// 步骤：填满 FIFO → 确认通知可重试 → 撤销排队入口 → 交付无客户端持有的登记。
void verify_backpressure_and_revoke() {
    CompletionNotifications notifications(1);
    FifoExecutor executor(1);
    OperationSource source(true);
    source.settle(OperationState::succeeded);
    int calls = 0;
    auto handle = notifications.subscribe(source.operation(), [&](const auto&) { ++calls; });
    executor.post([] {});
    check(notifications.schedule_one(executor) == ScheduleResult::blocked && notifications.read(handle) == NotificationState::pending,
        "executor pressure lost registration");
    check(executor.run_one() == RunResult::executed && notifications.schedule_one(executor) == ScheduleResult::queued,
        "notification could not retry scheduling");
    notifications.revoke(handle);
    notifications.release(handle);
    check(notifications.size() == 1 && executor.run_one() == RunResult::executed && calls == 0, "queued revoke still called");
    notifications.collect();
    check(!notifications.size() && source.operation()->cancellation_reason() == CancelReason::none, "revoke cancelled operation");
    handle = notifications.subscribe(source.operation(), [&](const auto&) { ++calls; });
    notifications.release(handle);
    check(notifications.schedule_one(executor) == ScheduleResult::queued && executor.run_one() == RunResult::executed && calls == 1,
        "released client dropped delivery");
    notifications.collect();
    check(!notifications.size() && !notifications.references(), "delivery without client leaked");

    handle = notifications.subscribe(source.operation(), [&](const auto&) { ++calls; });
    {
        FifoExecutor abandoned(1);
        check(notifications.schedule_one(abandoned) == ScheduleResult::queued, "temporary executor not queued");
    }
    check(notifications.read(handle) == NotificationState::pending && notifications.schedule_one(executor) == ScheduleResult::queued &&
        executor.run_one() == RunResult::executed && calls == 2, "discarded queue permanently stranded notification");
    notifications.release(handle);
}

// 验证消费者异常后的收尾，以及登记池销毁后排队包装无法再次进入消费者。
// 步骤：交付抛错消费者 → 检查终态与捕获回收 → 销毁已排队的登记池 → 推进空包装。
void verify_delivery_cleanup() {
    FifoExecutor executor(1);
    OperationSource source;
    source.settle(OperationState::failed);
    CompletionNotifications notifications(1);
    auto capture = std::make_shared<int>(1);
    const std::weak_ptr<int> lifetime = capture;
    const auto failed = notifications.subscribe(source.operation(), [capture](const auto&) { throw 7; });
    capture.reset();
    notifications.schedule_one(executor);
    check(executor.run_one() == RunResult::failed && notifications.read(failed) == NotificationState::delivered && lifetime.expired(),
        "throwing consumer leaked active entry");
    notifications.release(failed);
    bool called = false;
    {
        CompletionNotifications temporary(1);
        temporary.subscribe(source.operation(), [&](const auto&) { called = true; });
        temporary.schedule_one(executor);
    }
    check(executor.run_one() == RunResult::executed && !called && executor.run_one() == RunResult::idle,
        "destroyed pool left consumer entry");
}
}

// 验收生产通知登记、撤销及 FIFO 延后交付，不接入 DLL 或 C ABI 服务。
// 步骤：验证登记与持有 → 验证顺序及背压 → 验证收尾 → 汇报结果。
int main() {
    try {
        verify_registration();
        verify_ownership();
        verify_delivery_order();
        verify_backpressure_and_revoke();
        verify_delivery_cleanup();
        std::cout << "Runtime notification checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
