#include <dafeiyu/hooks.hpp>
#include <unordered_set>

namespace dfy::events {
using namespace runtime;
// 观察接纳失败与原业务隔离，成功回执只证明事件已被通道接纳。
// 步骤：尝试原子广播接纳 → 按容量、参数或其他错误返回独立回执。
ObservationReceipt observe(BroadcastChannel& channel, std::shared_ptr<const StructValue> payload) noexcept {
    try { return {ObservationStatus::accepted,channel.publish(std::move(payload))}; }
    catch (const ReferenceError& error) { return {error.reason() == ReferenceFailure::exhausted ? ObservationStatus::overloaded : ObservationStatus::failed,{}}; }
    catch (const std::length_error&) { return {ObservationStatus::overloaded,{}}; }
    catch (const std::bad_alloc&) { return {ObservationStatus::overloaded,{}}; }
    catch (const std::invalid_argument&) { return {ObservationStatus::invalid,{}}; }
    catch (...) { return {ObservationStatus::failed,{}}; }
}
// 固定控制参与者与全体允许策略，同一处理不能重复计入必要决策。
ControlGate::ControlGate(std::vector<std::shared_ptr<ProcessingRecord>> participants, TimePoint deadline, std::shared_ptr<Budget> budget)
    : participants_(std::move(participants)), group_(ResultStrategy::all_succeeded,participants_.size(),0,RemainingPolicy::request_cancel,std::move(budget)),
      deadline_(deadline) {
    std::unordered_set<const ProcessingRecord*> seen;
    for (const auto& participant : participants_)
        if (!participant || !seen.insert(participant.get()).second) throw std::invalid_argument("control gate: distinct participants required");
}
// 传播显式参与者取消，不取消普通观察流或无关业务。
void ControlGate::propagate(CancelReason reason) {
    for (std::size_t index = 0; index < participants_.size(); ++index)
        if (group_.result()->cancellation_requested(index)) participants_[index]->request_cancel(reason);
}
// 在宿主裁定点检查期限；已经确定的允许或否决不会被后到时钟和结果覆盖。
// 步骤：拒绝时钟倒退 → 按截止裁定未定决策 → 收集参与者唯一报告 → 传播未报告项取消。
void ControlGate::advance(TimePoint now) {
    if (last_ && now < *last_) throw std::invalid_argument("control gate: time moved backwards");
    last_ = now;
    if (operation()->state() == OperationState::pending && now >= deadline_) {
        group_.cancel(CancelReason::deadline); timed_out_ = true;
    }
    for (std::size_t index = 0; index < participants_.size(); ++index) {
        const auto state = participants_[index]->completion()->state();
        if (group_.result()->participant(index) == OperationState::pending && state != OperationState::pending) group_.report(index,state);
    }
    propagate(timed_out_ ? CancelReason::deadline : CancelReason::owner_stop);
}
// 取消意图先固定逻辑决策，再请求未报告处理退出，保持首个原因。
CancelDisposition ControlGate::cancel(CancelReason reason) {
    const auto result = group_.cancel(reason);
    if (result != CancelDisposition::already_settled) propagate(reason);
    return result;
}
// 只有全部必要控制处理成功才允许继续，失败或未知完成不被解释为许可。
ControlDecision ControlGate::decision() const noexcept {
    switch (operation()->state()) {
    case OperationState::pending: return ControlDecision::pending;
    case OperationState::succeeded: return ControlDecision::allowed;
    case OperationState::failed: return ControlDecision::denied;
    case OperationState::cancelled: return timed_out_ ? ControlDecision::timed_out : ControlDecision::cancelled;
    }
    return ControlDecision::denied;
}
// 逻辑决策与每个处理的真实退出分别查询，失败或超时不能代替退出确认。
bool ControlGate::exited() const noexcept {
    for (const auto& participant : participants_) if (participant->exit()->state() == OperationState::pending) return false;
    return true;
}
}
