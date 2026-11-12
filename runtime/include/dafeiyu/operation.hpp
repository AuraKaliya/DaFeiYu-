#pragma once
#include <memory>

namespace dfy::runtime {
// 宿主内部状态，不作为 C ABI 枚举值；终态只表达该操作约定的完成条件。
enum class OperationState { pending, succeeded, failed, cancelled };
class OperationSource;

// 消费者只读的操作记录，读取不推进任务，也不表示相关资源已经回收。
class Operation {
  public:
    // 返回独立状态副本，不增加引用或驱动宿主执行。
    OperationState state() const noexcept { return state_; }
    // 释放已无持有者的状态记录，不承担业务取消或异步清理。
    ~Operation() = default;
    // 禁止复制业务操作身份与状态。
    Operation(const Operation&) = delete;
    // 禁止赋值或移动操作记录。
    Operation& operator=(const Operation&) = delete;
  private:
    friend class OperationSource;
    // 只允许宿主完成源创建 Pending 记录。
    Operation() = default;
    OperationState state_ = OperationState::pending;
};

// 同一宿主线程使用的唯一完成源；宿主须独立持有它直至工作完成，不能依靠客户端引用。
class OperationSource {
  public:
    // 创建独立的 Pending 操作记录，分配失败不产生可观察操作。
    OperationSource();
    // 释放完成源持有，不隐式结算；宿主必须在销毁前结算已受理工作。
    ~OperationSource() = default;
    // 禁止复制完成权，多个客户端通过只读操作引用共同观察。
    OperationSource(const OperationSource&) = delete;
    // 禁止赋值或移动完成源。
    OperationSource& operator=(const OperationSource&) = delete;
    // 取得独立持有的只读操作记录；释放该引用不取消宿主工作。
    std::shared_ptr<const Operation> operation() const noexcept { return operation_; }
    // 从 Pending 设置唯一终态，非法状态或重复结算均拒绝且不改写记录。
    void settle(OperationState terminal);
  private:
    const std::shared_ptr<Operation> operation_;
};
}
