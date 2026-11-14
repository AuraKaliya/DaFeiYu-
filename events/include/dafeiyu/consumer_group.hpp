#pragma once
#include <dafeiyu/subscription_queue.hpp>

namespace dfy::events {
using MemberId = runtime::Handle;
// 同宿主线程内的有界消费者组，成员从共享队列单次领取，交付不表示业务处理完成。
class ConsumerGroup {
  public:
    // 建立独立成员域和共享交付队列，成员数与队列容量都必须为正。
    ConsumerGroup(std::shared_ptr<EventStore> store, std::size_t member_capacity, std::size_t queue_capacity);
    // 取消未领取交付并释放成员目录；已经交出的普通数据持有继续有效。
    ~ConsumerGroup() = default;
    // 禁止复制组身份、成员目录和领取责任。
    ConsumerGroup(const ConsumerGroup&) = delete;
    // 禁止赋值或隐式移动正在使用的组。
    ConsumerGroup& operator=(const ConsumerGroup&) = delete;
    // 为开放组签发成员身份，容量不足不改变已有成员；关闭后拒绝加入。
    MemberId join();
    // 撤销有效成员身份，不关闭共享队列，也不重放该成员已经领取的数据。
    void leave(MemberId member);
    // 判断成员身份是否仍有效，不改变目录或取得新的成员身份。
    bool has_member(MemberId member) const noexcept { return members_.contains(member); }
    // 由有效成员单次领取队首，空队列和流结束分别返回，不自动轮转其他成员。
    QueueRead poll(MemberId member);
    // 为组接纳一份事件引用，尚无成员时也可有界排队，满队列或存储不足原子拒绝。
    EventId publish(std::shared_ptr<const runtime::StructValue> payload);
    // 停止接纳并选择排空或取消；已有成员仍可领取排空数据。
    void close(CloseMode mode) { queue_.close(mode); }
    // 返回交付队列阶段，不表达成员处理结果或数据实际释放。
    QueueState state() const noexcept { return queue_.state(); }
    // 返回已经选择的关闭模式，开放组没有关闭原因。
    std::optional<CloseMode> close_mode() const noexcept { return queue_.close_mode(); }
    // 返回仍登记的成员数，已经领取的数据不计入成员数。
    std::size_t member_count() const noexcept { return members_.size(); }
    // 返回尚未领取的事件数，不包含已交出的数据持有。
    std::size_t queued() const noexcept { return queue_.size(); }
  private:
    friend class BroadcastChannel;
    struct Member {};
    std::shared_ptr<EventStore> store_;
    SubscriptionQueue queue_;
    runtime::HandleRegistry<const Member> members_;
};
}
