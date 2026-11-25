#include <dafeiyu/operation.hpp>
#include <dafeiyu/fifo_executor.hpp>
#include <dafeiyu/handle_registry.hpp>
#include <iostream>
#include <type_traits>

namespace {
using namespace dfy::runtime;
// 在状态迁移或工作持有责任违反契约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

// 验证各终态、独立快照及非法或重复结算的拒绝。
// 步骤：创建 Pending 记录 → 拒绝非法状态 → 分别结算终态 → 验证全部迟到结果不改写。
void verify_states() {
    for (const auto terminal : {OperationState::succeeded, OperationState::failed, OperationState::cancelled}) {
        OperationSource source;
        const auto view = source.operation();
        const auto before = view->state();
        for (const auto invalid : {OperationState::pending, static_cast<OperationState>(255)}) {
            bool rejected = false;
            try { source.settle(invalid); } catch (const std::invalid_argument&) { rejected = true; }
            check(rejected && view->state() == OperationState::pending, "invalid state accepted");
        }
        source.settle(terminal);
        check(before == OperationState::pending && view->state() == terminal, "snapshot or terminal changed");
        for (const auto late : {OperationState::succeeded, OperationState::failed, OperationState::cancelled}) {
            bool rejected = false;
            try { source.settle(late); } catch (const std::logic_error&) { rejected = true; }
            check(rejected && view->state() == terminal, "terminal overwritten");
        }
    }
}

// 验证客户端引用与宿主工作分别持有，释放客户端不隐式取消或丢弃完成责任。
// 步骤：建立独立引用 → 向生产执行器提交宿主工作 → 释放全部客户端引用 → 推进并验证最终回收。
void verify_work_ownership() {
    FifoExecutor executor(1);
    HandleRegistry<const Operation> references(2);
    auto source = std::make_shared<OperationSource>();
    const std::weak_ptr<const Operation> lifetime = source->operation();
    const auto first = references.insert(source->operation()), retained = references.retain(first);
    OperationState observed = OperationState::pending;
    executor.post([source, &observed] {
        source->settle(OperationState::succeeded);
        observed = source->operation()->state();
    });
    source.reset();
    references.release(first);
    check(references.get(retained).state() == OperationState::pending, "query advanced queued work");
    references.release(retained);
    check(!lifetime.expired() && observed == OperationState::pending, "client release dropped host work");
    check(executor.run_one() == RunResult::executed && observed == OperationState::succeeded && lifetime.expired(),
        "host work did not settle or release record");

    std::shared_ptr<const Operation> result;
    {
        OperationSource completed;
        result = completed.operation();
        completed.settle(OperationState::failed);
    }
    check(result->state() == OperationState::failed, "result disappeared with completion source");
}

// 验证首个取消原因、独立回执及所有终态都不被请求改写。
// 步骤：记录各类原因 → 重复请求保持原因 → 分别结算三种终态 → 验证快照与拒绝。
void verify_cancellation() {
    for (const auto reason : {CancelReason::user, CancelReason::owner_stop, CancelReason::deadline}) {
        for (const auto terminal : {OperationState::succeeded, OperationState::failed, OperationState::cancelled}) {
            OperationSource source(true);
            const auto view = source.operation(), retained = source.operation();
            const auto before = view->cancellation_reason();
            check(view->cancellable() && before == CancelReason::none, "unexpected initial cancellation");
            check(source.request_cancel(reason) == CancelDisposition::recorded && view->state() == OperationState::pending,
                "request incorrectly settled operation");
            const auto other = reason == CancelReason::user ? CancelReason::deadline : CancelReason::user;
            check(source.request_cancel(other) == CancelDisposition::already_requested &&
                retained->cancellation_reason() == reason && before == CancelReason::none, "first reason overwritten");
            source.settle(terminal);
            check(source.request_cancel(other) == CancelDisposition::already_settled &&
                view->state() == terminal && retained->cancellation_reason() == reason, "late cancellation changed terminal");
        }
    }
    OperationSource source(true);
    for (const auto invalid : {CancelReason::none, static_cast<CancelReason>(255)}) {
        bool rejected = false;
        try { (void)source.request_cancel(invalid); } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected && source.operation()->cancellation_reason() == CancelReason::none, "invalid reason changed record");
    }
    OperationSource unsupported;
    bool request_rejected = false, read_rejected = false;
    try { (void)unsupported.request_cancel(CancelReason::user); } catch (const std::domain_error&) { request_rejected = true; }
    try { (void)unsupported.operation()->cancellation_reason(); } catch (const std::domain_error&) { read_rejected = true; }
    check(request_rejected && read_rejected && !unsupported.operation()->cancellable() &&
        unsupported.operation()->state() == OperationState::pending, "unsupported cancellation accepted");
    unsupported.settle(OperationState::succeeded);
}

// 用生产执行器验证取消先到和完成先到，业务提交边界决定正式结果。
// 步骤：排队可取消工作 → 按先后顺序请求或推进 → 核对终态及原因快照。
void verify_cancel_order() {
    for (const bool cancel_first : {false, true}) {
        FifoExecutor executor(1);
        auto source = std::make_shared<OperationSource>(true);
        const auto view = source->operation();
        executor.post([source] {
            const bool cancelled = source->operation()->cancellation_reason() != CancelReason::none;
            source->settle(cancelled ? OperationState::cancelled : OperationState::succeeded);
        });
        if (cancel_first) check(source->request_cancel(CancelReason::user) == CancelDisposition::recorded, "cancel not recorded");
        check(executor.run_one() == RunResult::executed, "cancellable work failed");
        if (!cancel_first) check(source->request_cancel(CancelReason::user) == CancelDisposition::already_settled, "late cancel recorded");
        check(view->state() == (cancel_first ? OperationState::cancelled : OperationState::succeeded) &&
            view->cancellation_reason() == (cancel_first ? CancelReason::user : CancelReason::none), "cancel order ignored");
    }
}
}

// 验收生产操作的只读状态、协作取消和唯一终态，完成通知由后续单元提供。
// 步骤：核对权限接口 → 验证状态与寿命 → 验证取消及完成先后 → 汇报结果。
int main() {
    try {
        static_assert(!std::is_default_constructible_v<Operation> && !std::is_copy_constructible_v<OperationSource>);
        static_assert(std::is_same_v<decltype(std::declval<OperationSource&>().operation()), std::shared_ptr<const Operation>>);
        verify_states();
        verify_work_ownership();
        verify_cancellation();
        verify_cancel_order();
        std::cout << "Runtime operation checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
