#pragma once
#include <dafeiyu/operation.hpp>
#include <dafeiyu/value.hpp>
#include <dafeiyu/budget.hpp>
#include <optional>

namespace dfy::runtime {
enum class ResultStrategy { single, all_succeeded, any_succeeded, first_terminal };
enum class RemainingPolicy { keep_running, request_cancel };
class ResultGroup;
// 一组显式业务参与者的只读结果，普通观察者不进入该集合。
class GroupResult : public std::enable_shared_from_this<GroupResult> {
  public:
    // 禁止复制业务身份与汇总状态。
    GroupResult(const GroupResult&) = delete;
    // 禁止赋值或移动已发布结果。
    GroupResult& operator=(const GroupResult&) = delete;
    // 返回唯一汇总结果，不代表各参与者已经实际退出。
    std::shared_ptr<const Operation> operation() const noexcept { return source_.operation(); }
    // 返回决定结果的参与者；集体完成或主动取消没有单一胜出者。
    std::optional<std::size_t> winner() const noexcept { return winner_; }
    // 返回已报告的参与者数量，迟到报告可增加计数但不改写汇总结果。
    std::size_t reported() const noexcept { return reported_; }
    // 返回固定参与者数量，未报告的槽也属于本次契约。
    std::size_t size() const noexcept { return entries_.size(); }
    // 读取指定参与者状态，错误索引拒绝。
    OperationState participant(std::size_t index) const { return entries_.at(index).state; }
    // 读取宿主是否需要请求该参与者取消，不表示其已退出或撤销了外部效果。
    bool cancellation_requested(std::size_t index) const { return entries_.at(index).cancel; }
    // 取得保持完整结果和预算的值引用；尚未报告拒绝，无载荷返回空值。
    std::shared_ptr<const Value> value(std::size_t index) const;
  private:
    friend class ResultGroup;
    struct Entry { OperationState state = OperationState::pending; std::shared_ptr<const Value> value; bool cancel = false; };
    // 固定参与者槽，结果正文采用共享预算的预留上限。
    GroupResult(std::size_t participants, BudgetLease charge) : charge_(std::move(charge)), entries_(participants) {}
    // 预留最后归还，覆盖全部结果值的实际释放阶段。
    BudgetLease charge_;
    std::vector<Entry> entries_;
    OperationSource source_{true};
    std::optional<std::size_t> winner_;
    std::size_t reported_ = 0, bytes_ = 0;
    bool any_failed_ = false;
};

// 同宿主线程的有界结果完成源，报告顺序定义 first_terminal，不推测后台实际结束时刻。
class ResultGroup {
  public:
    // 固定策略、参与者数量及总正文上限；可在父预算预留全部结果槽与正文额度。
    ResultGroup(ResultStrategy strategy, std::size_t participants, std::size_t byte_limit,
        RemainingPolicy remaining = RemainingPolicy::keep_running, std::shared_ptr<Budget> parent_budget = {});
    // 禁止复制唯一结算权。
    ResultGroup(const ResultGroup&) = delete;
    // 禁止赋值或隐式移动已接纳的结果集合。
    ResultGroup& operator=(const ResultGroup&) = delete;
    // 返回独立只读结果，释放观察引用不会取消业务。
    std::shared_ptr<const GroupResult> result() const noexcept { return result_; }
    // 接受某参与者唯一终态及可选不可变值，迟到报告只补充自身记录。
    void report(std::size_t participant, OperationState terminal, std::shared_ptr<const Value> value = {});
    // 固定尚未结算业务的取消结果并标记未报告参与者需要取消，仍须分别等待其退出。
    CancelDisposition cancel(CancelReason reason);
  private:
    // 固定汇总终态及胜出者，并按显式策略标记仍未报告的参与者。
    void settle(OperationState terminal, std::optional<std::size_t> winner);
    const ResultStrategy strategy_;
    const RemainingPolicy remaining_;
    const std::size_t byte_limit_;
    std::shared_ptr<GroupResult> result_;
};
}
