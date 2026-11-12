#include <dafeiyu/operation.hpp>
#include <stdexcept>

namespace dfy::runtime {
// 由宿主完成源独立持有状态记录，客户端的保留和释放不转移完成权。
OperationSource::OperationSource() : operation_(new Operation) {}

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
