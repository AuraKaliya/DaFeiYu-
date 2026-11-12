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
}

// 验收有界通知登记与撤销，本单元尚不派发回调或接入 DLL。
// 步骤：验证登记边界 → 验证持有责任 → 汇报结果。
int main() {
    try {
        verify_registration();
        verify_ownership();
        std::cout << "Runtime notification checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
