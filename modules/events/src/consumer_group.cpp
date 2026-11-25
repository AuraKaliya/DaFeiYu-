#include <dafeiyu/consumer_group.hpp>

namespace dfy::events {
// 保存共享存储并建立固定队列及成员预算，失败不发布部分组对象。
ConsumerGroup::ConsumerGroup(std::shared_ptr<EventStore> store, std::size_t member_capacity, std::size_t queue_capacity)
    : store_(std::move(store)), queue_(store_,queue_capacity), members_(member_capacity) {}
// 在共享队列仍开放时登记独立成员，标识的数值复制不产生第二个成员。
// 步骤：核对开放状态 → 签发唯一成员身份。
MemberId ConsumerGroup::join() {
    if (queue_.state() != QueueState::open) throw std::logic_error("group: joining closed admission");

    return members_.insert(std::make_shared<const Member>(),false);
}
// 只撤销当前成员登记，已领取事件和其他成员保持原状态。
void ConsumerGroup::leave(MemberId member) {
    (void)members_.get(member);
    members_.release(member);
}
// 验证成员身份后原子转交队首，后续成员不能再次领取同一份交付。
QueueRead ConsumerGroup::poll(MemberId member) {
    (void)members_.get(member);
    return queue_.poll();
}
// 接纳到组共享队列，一份事件只占一份存储引用，不按成员数广播副本。
// 步骤：校验载荷与开放容量 → 原子准备事件引用 → 无失败点提交组队列 → 返回接纳身份。
EventId ConsumerGroup::publish(std::shared_ptr<const runtime::StructValue> payload) {
    if (!payload) throw std::invalid_argument("group: payload required");
    if (queue_.state() != QueueState::open) throw std::logic_error("group: admission closed");
    if (!queue_.available()) throw std::length_error("group: queue full");

    auto references = store_->accept_batch(std::move(payload),1);
    const auto id = store_->get(references.front()).id;
    queue_.append_owned(references.front());
    return id;
}
}
