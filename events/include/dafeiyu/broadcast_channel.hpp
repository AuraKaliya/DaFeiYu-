#pragma once
#include <dafeiyu/subscription_queue.hpp>
#include <span>

namespace dfy::events {
// 同一宿主线程内的有界交付通道，支持全部开放接收方或明确目标的原子接纳。
class BroadcastChannel {
  public:
    // 指定共享事件存储、正订阅容量及每订阅固定队列容量。
    BroadcastChannel(std::shared_ptr<EventStore> store, std::size_t subscriptions, std::size_t queue_capacity);
    // 取消仍保留的交付队列，不撤销消费者已经读出的普通数据。
    ~BroadcastChannel();
    // 禁止复制接收方目录与广播责任。
    BroadcastChannel(const BroadcastChannel&) = delete;
    // 禁止赋值或隐式移动正在使用的广播通道。
    BroadcastChannel& operator=(const BroadcastChannel&) = delete;
    // 创建由通道保持的独立接收队列，调用方引用消失不自动移除订阅。
    std::shared_ptr<SubscriptionQueue> subscribe();
    // 移除本通道中已 Closed 的订阅，旧队列对象仍可读取结束状态。
    void remove(const std::shared_ptr<SubscriptionQueue>& subscription);
    // 原子接纳到当前全部 Open 队列，返回事件身份；不等待消费者处理。
    EventId publish(std::shared_ptr<const runtime::StructValue> payload);
    // 原子接纳到非空且唯一的本通道 Open 目标；未知、关闭或任一目标满时全部拒绝。
    EventId publish_to(std::shared_ptr<const runtime::StructValue> payload,
        std::span<const std::shared_ptr<SubscriptionQueue>> targets);
    // 返回当前登记的订阅数量，Closed 项在显式移除前也占名额。
    std::size_t size() const noexcept { return size_; }
  private:
    // 为已预检的目标准备事件引用并无失败点交付；空目标仅供普通广播记录接纳。
    EventId deliver(std::shared_ptr<const runtime::StructValue> payload,
        std::span<const std::shared_ptr<SubscriptionQueue>> targets);
    std::shared_ptr<EventStore> store_;
    std::vector<std::shared_ptr<SubscriptionQueue>> subscriptions_;
    const std::size_t queue_capacity_;
    std::size_t size_ = 0;
};
}
