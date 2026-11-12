#include <dafeiyu/operation.hpp>
#include <stdexcept>

namespace dfy::runtime {
// 由宿主完成源独立持有状态记录，客户端的保留和释放不转移完成权。
OperationSource::OperationSource(bool cancellable) : operation_(new Operation(cancellable)) {}

// 返回独立的首个取消原因快照，不推动任务或更改状态。
CancelReason Operation::cancellation_reason() const {
    if (!cancellable_) throw std::domain_error("operation: cancellation unsupported");
    return cancellation_reason_;
}

// 记录协作取消请求，先到的正式终态和首个有效原因均不被改写。
// 步骤：校验原因与支持性 → 区分已结算及重复请求 → 保存首个原因。
CancelDisposition OperationSource::request_cancel(CancelReason reason) {
    if (reason != CancelReason::user && reason != CancelReason::owner_stop && reason != CancelReason::deadline) {
        throw std::invalid_argument("operation: valid cancellation reason required");
    }
    if (!operation_->cancellable_) throw std::domain_error("operation: cancellation unsupported");
    if (operation_->state_ != OperationState::pending) return CancelDisposition::already_settled;
    if (operation_->cancellation_reason_ != CancelReason::none) return CancelDisposition::already_requested;
    operation_->cancellation_reason_ = reason;
    return CancelDisposition::recorded;
}

// 固定操作唯一终态，不把重复成功或迟到结果当作幂等结算。
// 步骤：校验终态种类 → 拒绝已结算记录 → 写入唯一正式结果。
void OperationSource::settle(OperationState terminal) {
    if (terminal != OperationState::succeeded && terminal != OperationState::failed && terminal != OperationState::cancelled) {
        throw std::invalid_argument("operation: terminal state required");
    }
    if (operation_->state_ != OperationState::pending) throw std::logic_error("operation: already settled");
    operation_->state_ = terminal;
}
}
