#pragma once
#include <cstddef>
#include <functional>
#include <vector>

namespace dfy::runtime {
enum class RunResult { idle, executed, failed };

// 同一宿主线程使用的有界 FIFO；容量计排队任务，另最多有一个正在执行的任务。
// 宿主须覆盖全部执行期；捕获资源析构不得重入执行器，业务取消和 DLL 归属由上层管理。
class FifoExecutor {
  public:
    using Task = std::function<void()>;
    // 分配固定任务槽，拒绝零容量；分配失败不返回执行器。
    explicit FifoExecutor(std::size_t capacity);
    // 释放未执行任务的捕获资源；外层服务须先处理业务退出及结算责任。
    ~FifoExecutor() = default;
    // 禁止复制待执行任务及推进状态。
    FifoExecutor(const FifoExecutor&) = delete;
    // 禁止赋值或隐式移动正在被宿主使用的执行器。
    FifoExecutor& operator=(const FifoExecutor&) = delete;
    // 只登记非空任务；满队列抛出 length_error，不调用任务正文。
    void post(Task task);
    // 推进一个队首任务并隔离其异常，递归推进抛出 logic_error；回调内提交留待后续推进。
    RunResult run_one();
    // 返回排队任务数，不含当前调用。
    std::size_t size() const noexcept { return size_; }
    // 返回固定排队容量。
    std::size_t capacity() const noexcept { return slots_.size(); }
    // 判断是否处于任务调用及其捕获资源释放阶段。
    bool running() const noexcept { return running_; }

  private:
    std::vector<Task> slots_;
    std::size_t head_ = 0, tail_ = 0, size_ = 0;
    bool running_ = false;
};
}
