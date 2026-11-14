#pragma once
#include <dafeiyu/operation.hpp>
#include <dafeiyu/runtime_ids.hpp>
#include <dafeiyu/budget.hpp>
#include <chrono>
#include <optional>
#include <vector>

namespace dfy::runtime {
using RetryTime = std::chrono::steady_clock::time_point;
struct RetryPolicy {
    std::size_t maximum_attempts = 1;
    std::chrono::milliseconds initial_delay{100}, maximum_delay{5000};
};
enum class RetryPhase { ready, running, backoff, finished };
struct AttemptId {
    DomainId domain = 0; std::uint64_t number = 0;
    // 比较具体重试业务域及尝试序号，不把旧尝试结果应用到新尝试。
    bool operator==(const AttemptId&) const = default;
};
struct AttemptSnapshot { AttemptId id; OperationState state; RetryTime started; std::optional<RetryTime> exited; };

// 同宿主线程的有界重试协调器；保持原业务身份，尝试实际退出后才允许创建下一次执行。
class RetryController {
  public:
    // 预分配全部尝试槽及就绪操作，最多 64 次；可预留元数据父预算，截止不早于接纳。
    RetryController(RetryPolicy policy, RetryTime accepted, RetryTime deadline, std::shared_ptr<Budget> budget = {});
    // 取消未运行的等待；运行时宿主必须保留控制器，析构不伪造尝试退出。
    ~RetryController();
    // 禁止复制重试完成权与尝试身份。
    RetryController(const RetryController&) = delete;
    // 禁止赋值或移动在途重试控制器。
    RetryController& operator=(const RetryController&) = delete;
    // 返回最终业务操作；期限或取消可先确定结果，实际运行尝试仍须 finish_attempt。
    std::shared_ptr<const Operation> operation() const noexcept { return result_.operation(); }
    // 登记首个取消原因，下一次推进裁定，已经结束的业务不重新打开。
    CancelDisposition request_cancel(CancelReason reason) { return result_.request_cancel(reason); }
    // 推进单调时钟、截止与退避就绪，不运行尝试、不调用消费者。
    void advance(RetryTime now);
    // 返回下一尝试的就绪操作，运行中或全部结束时为空；取消等待也会唤醒已取得的操作。
    std::shared_ptr<const Operation> readiness() const noexcept;
    // 在明确调度机会内开始一次就绪尝试，未就绪或结果已确定返回空，不自动发请求。
    std::optional<AttemptId> begin_attempt(RetryTime now);
    // 确认当前尝试已实际退出并报告唯一终态；只有明确安全的失败且预算剩余才进入退避。
    void finish_attempt(AttemptId attempt, OperationState terminal, bool safe_to_retry, RetryTime now);
    // 返回实际开始过的尝试快照，未开始槽不能伪装为历史。
    AttemptSnapshot attempt(std::size_t index) const;
    // 返回开始过的尝试数，预留和退避不增加该计数。
    std::size_t attempts() const noexcept { return started_; }
    // 返回阶段，业务取消或超时后 running 仍表示实际尝试未退出。
    RetryPhase phase() const noexcept { return phase_; }
  private:
    struct Attempt {
        OperationSource ready;
        AttemptSnapshot snapshot{};
    };
    // 取消尚未开始的就绪等待，保留已经授予的就绪事实。
    void finish_waiting();
    const DomainId domain_;
    const RetryPolicy policy_;
    const RetryTime deadline_;
    RetryTime last_, due_;
    std::chrono::milliseconds delay_;
    std::vector<std::unique_ptr<Attempt>> entries_;
    BudgetLease charge_;
    OperationSource result_{true};
    std::size_t started_ = 0;
    RetryPhase phase_ = RetryPhase::ready;
};
}
