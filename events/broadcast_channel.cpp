#include <dafeiyu/broadcast_channel.hpp>
#include <algorithm>
#include <unordered_set>

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

    return deliver(std::move(payload),targets);
}

// 只向明确指定的本通道接收方交付，其他队列的背压或关闭不影响本次选择。
// 步骤：校验非空唯一目标 → 按目录固定实际接收方 → 核对开放状态及容量 → 原子交付。
EventId BroadcastChannel::publish_to(std::shared_ptr<const runtime::StructValue> payload,
    std::span<const std::shared_ptr<SubscriptionQueue>> targets) {
    if (!payload || targets.empty()) throw std::invalid_argument("broadcast: payload and targets required");
    if (targets.size() > subscriptions_.size()) throw std::length_error("broadcast: target count exceeds capacity");
    std::unordered_set<const SubscriptionQueue*> requested;
    requested.reserve(targets.size());
    for (const auto& target : targets) {
        if (!target || !requested.insert(target.get()).second) throw std::invalid_argument("broadcast: null or duplicate target");
    }

    std::vector<std::shared_ptr<SubscriptionQueue>> selected;
    selected.reserve(targets.size());
    for (const auto& subscription : subscriptions_) {
        if (subscription && requested.erase(subscription.get())) selected.push_back(subscription);
    }
    if (!requested.empty()) throw std::invalid_argument("broadcast: unknown target");
    for (const auto& target : selected) {
        if (target->state() != QueueState::open) throw std::logic_error("broadcast: target not open");
        if (!target->available()) throw std::length_error("broadcast: target queue full");
    }

    return deliver(std::move(payload),selected);
}

// 对已经固定且预检通过的目标提交同一个事件身份，失败不留下部分交付。
// 步骤：原子准备事件引用 → 读取接纳身份 → 无失败点转交队列 → 处理无接收方广播。
EventId BroadcastChannel::deliver(std::shared_ptr<const runtime::StructValue> payload,
    std::span<const std::shared_ptr<SubscriptionQueue>> targets) {

    auto references = store_->accept_batch(std::move(payload), targets.empty() ? 1 : targets.size());
    const auto id = store_->get(references.front()).id;
    for (std::size_t index = 0; index < targets.size(); ++index) targets[index]->append_owned(references[index]);
    if (targets.empty()) { store_->release(references.front()); references.front() = {}; }
    return id;
}
}
