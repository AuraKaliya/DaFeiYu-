#include <dafeiyu/fifo_executor.hpp>
#include <stdexcept>

namespace dfy::runtime {
// 建立固定槽位的空队列，零容量无法接纳任务而被明确拒绝。
FifoExecutor::FifoExecutor(std::size_t capacity) : slots_(capacity) {
    if (!capacity) throw std::invalid_argument("executor: positive capacity required");
}

// 将任务转交队尾，拒绝时保留所有已接纳任务及原有顺序。
// 步骤：校验任务及容量 → 无抛出交换任务 → 推进队尾与计数。
void FifoExecutor::post(Task task) {
    if (!task) throw std::invalid_argument("executor: nonempty task required");
    if (size_ == slots_.size()) throw std::length_error("executor: queue full");
    slots_[tail_].swap(task);
    tail_ = (tail_ + 1) % slots_.size();
    ++size_;
}

// 执行一个 FIFO 任务，异常仅形成当前推进结果，不丢弃剩余队列。
// 步骤：拒绝重入或返回空闲 → 移出队首并开放槽位 → 执行并隔离异常 → 释放捕获后恢复空闲。
RunResult FifoExecutor::run_one() {
    if (running_) throw std::logic_error("executor: reentrant advance");
    if (!size_) return RunResult::idle;

    auto result = RunResult::executed;
    running_ = true;
    {
        Task task;
        task.swap(slots_[head_]);
        head_ = (head_ + 1) % slots_.size();
        --size_;

        try { task(); } catch (...) { result = RunResult::failed; }
        // 捕获资源在 running_ 保护内释放，不能将调用返回误当成代码依赖已经结束。
    }
    running_ = false;
    return result;
}
}
