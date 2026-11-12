#pragma once
#include <dafeiyu/subscription_queue.hpp>

namespace dfy::events {
// 同一宿主线程内的有界独立广播，接纳时固定全部 Open 接收方，全接纳或全拒绝。
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
    // 返回当前登记的订阅数量，Closed 项在显式移除前也占名额。
    std::size_t size() const noexcept { return size_; }
  private:
    std::shared_ptr<EventStore> store_;
    std::vector<std::shared_ptr<SubscriptionQueue>> subscriptions_;
    const std::size_t queue_capacity_;
    std::size_t size_ = 0;
};
}
