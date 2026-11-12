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
}

// 验收生产操作的只读视图和唯一终态，取消请求与完成通知由后续单元提供。
// 步骤：核对权限接口 → 验证状态迁移 → 验证工作与引用寿命 → 汇报结果。
int main() {
    try {
        static_assert(!std::is_default_constructible_v<Operation> && !std::is_copy_constructible_v<OperationSource>);
        static_assert(std::is_same_v<decltype(std::declval<OperationSource&>().operation()), std::shared_ptr<const Operation>>);
        verify_states();
        verify_work_ownership();
        std::cout << "Runtime operation checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
