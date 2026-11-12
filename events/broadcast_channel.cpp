#include <dafeiyu/broadcast_channel.hpp>
#include <algorithm>

namespace dfy::events {
// 校验接纳预算后建立固定订阅槽，失败不创建半个订阅。
// 步骤：保存宿主存储 → 校验正容量 → 分配订阅槽。
BroadcastChannel::BroadcastChannel(std::shared_ptr<EventStore> store, std::size_t subscriptions, std::size_t queue_capacity)
    : store_(std::move(store)), queue_capacity_(queue_capacity) {
    if (!store_ || !subscriptions || !queue_capacity) throw std::invalid_argument("broadcast: store and positive capacities required");
    subscriptions_.resize(subscriptions);
}

// 结束全部尚未关闭的接收队列，外部数据凭证继续独立存活。
BroadcastChannel::~BroadcastChannel() {
    for (const auto& subscription : subscriptions_) if (subscription) subscription->close(CloseMode::cancel);
}

// 在固定目录中创建独立队列，容量不足不改变已有接收方。
// 步骤：查找空槽 → 完整创建队列 → 提交目录记录并返回共享控制持有。
std::shared_ptr<SubscriptionQueue> BroadcastChannel::subscribe() {
    for (auto& subscription : subscriptions_) {
        if (subscription) continue;
        auto created = std::make_shared<SubscriptionQueue>(store_, queue_capacity_);
        subscription = created;
        ++size_;
        return created;
    }
    throw std::length_error("broadcast: subscription capacity exhausted");
}

// 仅移除本通道已经结束的订阅，不把移除请求解释为消费者任务已经退出。
// 步骤：核对归属及 Closed → 释放目录持有 → 归还订阅名额。
void BroadcastChannel::remove(const std::shared_ptr<SubscriptionQueue>& subscription) {
    const auto found = std::find(subscriptions_.begin(), subscriptions_.end(), subscription);
    if (!subscription || found == subscriptions_.end()) throw std::invalid_argument("broadcast: unknown subscription");
    if (subscription->state() != QueueState::closed) throw std::logic_error("broadcast: subscription not closed");
    found->reset();
    --size_;
}

// 接纳一份共享不可变事件到全部开放接收方，任何预检或引用准备失败都不交付。
// 步骤：固定接收方并检查各队列 → 原子准备事件引用 → 无失败点提交各队列 → 返回接纳身份。
EventId BroadcastChannel::publish(std::shared_ptr<const runtime::StructValue> payload) {
    if (!payload) throw std::invalid_argument("broadcast: payload required");
    std::vector<std::shared_ptr<SubscriptionQueue>> targets;
    targets.reserve(subscriptions_.size());
    for (const auto& subscription : subscriptions_) {
        if (!subscription || subscription->state() != QueueState::open) continue;
        if (!subscription->available()) throw std::length_error("broadcast: receiver queue full");
        targets.push_back(subscription);
    }

    auto references = store_->accept_batch(std::move(payload), targets.empty() ? 1 : targets.size());
    const auto id = store_->get(references.front()).id;
    for (std::size_t index = 0; index < targets.size(); ++index) targets[index]->append_owned(references[index]);
    if (targets.empty()) { store_->release(references.front()); references.front() = {}; }
    return id;
}
}
