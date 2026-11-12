#include <dafeiyu/fifo_executor.hpp>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
using namespace dfy::runtime;
// 在推进顺序或任务寿命违反契约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

// 验证延后调用、FIFO、回调中提交与递归推进拒绝。
// 步骤：填充队列 → 推进首项并追加 → 核对后续顺序 → 反复复用环形槽位。
void verify_order() {
    FifoExecutor executor(2);
    std::vector<int> trace;
    bool nested_rejected = false, full_rejected = false;
    // 在任务内追加新工作并检查推进保护，所有新调用必须留到宿主下一次推进。
    // 步骤：记录活动状态 → 追加队尾 → 尝试满队列提交和递归推进。
    executor.post([&] {
        check(executor.running() && executor.size() == 1, "active task incorrectly counted");
        trace.push_back(1);
        executor.post([&] { trace.push_back(3); });
        try { executor.post([&] { trace.push_back(99); }); } catch (const std::length_error&) { full_rejected = true; }
        try { (void)executor.run_one(); } catch (const std::logic_error&) { nested_rejected = true; }
    });
    executor.post([&] { trace.push_back(2); });
    check(trace.empty() && executor.size() == 2 && executor.capacity() == 2, "post executed task");
    check(executor.run_one() == RunResult::executed && trace == std::vector<int>{1}, "first advance executed too much");
    check(nested_rejected && full_rejected && !executor.running() && executor.size() == 2, "rejection changed queue");
    check(executor.run_one() == RunResult::executed && executor.run_one() == RunResult::executed &&
        trace == std::vector<int>({1, 2, 3}) && executor.run_one() == RunResult::idle, "FIFO order lost");

    for (int i = 0; i < 32; ++i) {
        executor.post([&, i] { trace.push_back(i + 4); });
        check(executor.run_one() == RunResult::executed && trace.back() == i + 4, "ring slot reuse failed");
    }
}

// 验证失败任务的捕获回收、后续推进和未执行任务的所有权。
// 步骤：提交持有资源的失败任务 → 验证调用期及返回后的寿命 → 推进后继 → 销毁未执行队列。
void verify_failure_and_lifetime() {
    FifoExecutor executor(2);
    auto resource = std::make_shared<int>(42);
    const std::weak_ptr<int> lifetime = resource;
    executor.post([resource, &executor] {
        check(executor.running() && *resource == 42, "capture lost before invocation");
        throw 7;
    });
    resource.reset();
    bool next = false;
    executor.post([&] { next = true; });
    check(!lifetime.expired() && executor.run_one() == RunResult::failed && lifetime.expired(), "failed task leaked");
    check(!executor.running() && !next && executor.size() == 1, "failure consumed other work");
    check(executor.run_one() == RunResult::executed && next, "failure prevented later work");

    auto pending = std::make_shared<int>(1);
    const std::weak_ptr<int> pending_lifetime = pending;
    bool called = false;
    {
        FifoExecutor temporary(1);
        temporary.post([pending, &called] { called = true; });
        pending.reset();
        check(!pending_lifetime.expired(), "queue did not own capture");
    }
    check(pending_lifetime.expired() && !called, "queue destruction executed or leaked task");
    bool empty_rejected = false, zero_rejected = false;
    try { executor.post({}); } catch (const std::invalid_argument&) { empty_rejected = true; }
    try { FifoExecutor zero(0); } catch (const std::invalid_argument&) { zero_rejected = true; }
    check(empty_rejected && zero_rejected && executor.run_one() == RunResult::idle, "invalid submission changed state");
}
}

// 验收生产 FIFO 的接纳、推进和捕获寿命，不把执行结果当作业务终态。
// 步骤：检查延后顺序 → 检查失败及资源 → 汇报结果。
int main() {
    try {
        verify_order();
        verify_failure_and_lifetime();
        std::cout << "Runtime executor checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
