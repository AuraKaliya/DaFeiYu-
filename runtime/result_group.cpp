#include <dafeiyu/result_group.hpp>

namespace dfy::runtime {
// 在发布结果前核对完整契约并预留容量，失败自动归还已申请额度。
// 步骤：校验策略及数量 → 申请可选共享预算 → 创建固定结果槽。
ResultGroup::ResultGroup(ResultStrategy strategy, std::size_t participants, std::size_t byte_limit,
    RemainingPolicy remaining, std::shared_ptr<Budget> parent_budget)
    : strategy_(strategy), remaining_(remaining), byte_limit_(byte_limit) {
    if (strategy != ResultStrategy::single && strategy != ResultStrategy::all_succeeded && strategy != ResultStrategy::any_succeeded &&
        strategy != ResultStrategy::first_terminal) throw std::invalid_argument("result group: invalid strategy");
    if (!participants || participants > 65536 || (strategy == ResultStrategy::single && participants != 1) ||
        (remaining != RemainingPolicy::keep_running && remaining != RemainingPolicy::request_cancel))
        throw std::invalid_argument("result group: invalid participants or remaining policy");
    auto charge = parent_budget ? parent_budget->acquire({participants,byte_limit}) : BudgetLease{};
    result_ = std::shared_ptr<GroupResult>(new GroupResult(participants,std::move(charge)));
}
// 保留整个结果对象作为别名所有者，不能通过提取值逃逸已预留预算。
std::shared_ptr<const Value> GroupResult::value(std::size_t index) const {
    const auto& entry = entries_.at(index);
    if (entry.state == OperationState::pending) throw std::logic_error("result group: participant pending");
    return entry.value ? std::shared_ptr<const Value>(shared_from_this(),entry.value.get()) : nullptr;
}
// 一次性确定汇总结果，未退出工作的取消责任仍由宿主执行。
void ResultGroup::settle(OperationState terminal, std::optional<std::size_t> winner) {
    result_->source_.settle(terminal); result_->winner_ = winner;
    if (remaining_ == RemainingPolicy::request_cancel)
        for (auto& entry : result_->entries_) if (entry.state == OperationState::pending) entry.cancel = true;
}
// 接受唯一参与者报告，先验证全部数据再提交，既定业务终态不被迟到结果覆盖。
// 步骤：核对索引、终态及正文预算 → 固定参与者记录 → 保留既定汇总或按策略裁定。
void ResultGroup::report(std::size_t participant, OperationState terminal, std::shared_ptr<const Value> value) {
    auto& entry = result_->entries_.at(participant);
    if (terminal != OperationState::succeeded && terminal != OperationState::failed && terminal != OperationState::cancelled)
        throw std::invalid_argument("result group: terminal state required");
    if (entry.state != OperationState::pending) throw std::logic_error("result group: participant already reported");
    const auto bytes = value ? value->payload_bytes() : 0;
    if (bytes > byte_limit_ - result_->bytes_) throw std::length_error("result group: byte budget exhausted");
    entry.value = std::move(value); entry.state = terminal; ++result_->reported_; result_->bytes_ += bytes;
    result_->any_failed_ |= terminal == OperationState::failed;
    if (result_->operation()->state() != OperationState::pending) return;

    switch (strategy_) {
    case ResultStrategy::single:
    case ResultStrategy::first_terminal: settle(terminal,participant); break;
    case ResultStrategy::all_succeeded:
        if (terminal != OperationState::succeeded) settle(terminal,participant);
        else if (result_->reported_ == result_->size()) settle(OperationState::succeeded,{});
        break;
    case ResultStrategy::any_succeeded:
        if (terminal == OperationState::succeeded) settle(terminal,participant);
        else if (result_->reported_ == result_->size()) settle(result_->any_failed_ ? OperationState::failed : OperationState::cancelled,{});
        break;
    }
}
// 主动取消只固定本组逻辑结果，迟到成功仍可记录为参与者事实但不能复活业务。
CancelDisposition ResultGroup::cancel(CancelReason reason) {
    const auto disposition = result_->source_.request_cancel(reason);
    if (disposition == CancelDisposition::already_settled) return disposition;
    settle(OperationState::cancelled,{});
    for (auto& entry : result_->entries_) if (entry.state == OperationState::pending) entry.cancel = true;
    return disposition;
}
}
