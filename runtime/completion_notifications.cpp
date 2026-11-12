#include <dafeiyu/completion_notifications.hpp>

namespace dfy::runtime {
// 分配登记槽和独立域引用表，构造失败不产生对外登记。
CompletionNotifications::CompletionNotifications(std::size_t capacity) : records_(capacity), references_(capacity) {}

// 关闭全部未开始入口，排队的宿主包装继续存活但只能跳过已撤销记录。
CompletionNotifications::~CompletionNotifications() {
    revoke_pending();
}

// 按所有者收尾未开始的通知，排队的宿主包装随后仅跳过。
void CompletionNotifications::revoke_pending() noexcept {
    for (auto& record : records_) {
        if (record && record->state == NotificationState::pending) finish(*record, NotificationState::revoked);
    }
}

// 先结束可能进入消费者代码的捕获及操作借用，再公开终结状态。
void CompletionNotifications::finish(Record& record, NotificationState terminal) noexcept {
    record.callback = {};
    record.operation.reset();
    record.state = terminal;
}

// 将最早的可交付通知登记为 FIFO 工作，不在调度提交中调用消费者。
// 步骤：回收终结项 → 按登记顺序选择完成操作 → 核对执行器容量 → 提交并标记排队。
ScheduleResult CompletionNotifications::schedule_one(FifoExecutor& executor) {
    collect();
    std::shared_ptr<Record> selected;
    for (const auto& record : records_) {
        if (!record || record->state != NotificationState::pending || record->queued ||
            record->operation->state() == OperationState::pending) continue;
        if (!selected || record->order < selected->order) selected = record;
    }
    if (!selected) return ScheduleResult::idle;
    if (executor.size() == executor.capacity()) return ScheduleResult::blocked;
    auto scheduled = std::make_shared<Scheduled>(selected);
    executor.post([scheduled] { deliver(scheduled->record); });
    selected->queued = true;
    return ScheduleResult::queued;
}

// 交付已结算操作的通知一次，撤销不会取消操作，释放客户端引用不会丢失这次工作。
// 步骤：过滤已撤销项 → 标记活动 → 借用操作调用消费者 → 按成功或异常路径收尾。
void CompletionNotifications::deliver(const std::shared_ptr<Record>& record) {
    if (record->state != NotificationState::pending) return;
    record->state = NotificationState::running;
    try { record->callback(record->operation); }
    catch (...) { finish(*record, NotificationState::delivered); throw; }
    finish(*record, NotificationState::delivered);
}

// 在有界槽位中接纳通知责任，引用签发成功后再提交登记和序号。
// 步骤：校验输入 → 回收终结记录并检查容量 → 准备操作及回调持有 → 签发引用并提交。
Handle CompletionNotifications::subscribe(std::shared_ptr<const Operation> operation, Callback callback) {
    if (!operation || !callback) throw std::invalid_argument("notification: operation and callback required");
    collect();
    if (size_ == records_.size() || !next_order_) throw ReferenceError(ReferenceFailure::exhausted);
    auto record = std::make_shared<Record>(Record{std::move(operation), std::move(callback), next_order_});
    const auto registration = references_.insert(std::make_shared<Reference>(Reference{record}), false);
    for (auto& slot : records_) {
        if (slot) continue;
        slot = std::move(record);
        break;
    }
    ++size_;
    next_order_ = next_order_ == std::numeric_limits<std::uint64_t>::max() ? 0 : next_order_ + 1;
    return registration;
}

// 撤销单条登记，不取消其底层操作或其他等待者。
// 步骤：验证引用 → 按活动或终结状态分流 → 撤销 Pending 并释放回调及操作持有。
RevokeResult CompletionNotifications::revoke(Handle registration) {
    const auto& record = references_.get(registration).record;
    if (record->state == NotificationState::running) return RevokeResult::busy;
    if (record->state != NotificationState::pending) return RevokeResult::already_inactive;
    finish(*record, NotificationState::revoked);
    return RevokeResult::revoked;
}

// 仅释放客户端持有并回收终结记录，待通知责任仍由登记槽保持。
void CompletionNotifications::release(Handle registration) {
    references_.release(registration);
    collect();
}

// 回收已终结且只剩池持有的记录，保留客户端和后续执行所需的状态。
// 步骤：筛选终结状态 → 核对独立持有数量 → 清空槽并归还登记容量。
void CompletionNotifications::collect() noexcept {
    for (auto& record : records_) {
        if (!record || (record->state != NotificationState::delivered && record->state != NotificationState::revoked)) continue;
        if (record.use_count() != 1) continue;
        record.reset();
        --size_;
    }
}
}
