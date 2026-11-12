#include <dafeiyu/subscription_queue.hpp>

namespace dfy::events {
// 移动数据持有，避免覆盖原引用时遗漏归还。
// 步骤：处理自移动 → 归还旧引用 → 转交存储及来源标识。
EventLease& EventLease::operator=(EventLease&& other) noexcept {
    if (this == &other) return *this;
    reset();
    store_ = std::move(other.store_);
    owned_ = std::exchange(other.owned_, {});
    return *this;
}

// 只读借用当前数据，空持有不提供伪造事件。
const EventRecord& EventLease::get() const {
    if (!store_) throw std::logic_error("event: empty lease");
    return store_->get(owned_);
}

// 显式保持同一事件，保留失败不改变原引用。
EventLease EventLease::retain() const {
    if (!store_) throw std::logic_error("event: empty lease");
    return EventLease(store_, store_->retain(owned_));
}

// 归还内部唯一拥有的事件引用，不产生投递或业务完成结果。
void EventLease::reset() noexcept {
    if (!store_) return;
    store_->release(owned_);
    owned_ = {};
    store_.reset();
}

// 建立独立固定槽位队列，事件存储覆盖排队及读出数据的寿命。
SubscriptionQueue::SubscriptionQueue(std::shared_ptr<EventStore> store, std::size_t capacity)
    : store_(std::move(store)), slots_(capacity) {
    if (!store_ || !capacity) throw std::invalid_argument("subscription: store and positive capacity required");
}

// 在接纳和容量均允许时保留事件，提交之后来源引用仍由调用方持有。
// 步骤：核对接纳及容量 → 保留独立事件引用 → 写入队尾并更新数量。
void SubscriptionQueue::push(runtime::Handle source) {
    if (state_ != QueueState::open) throw std::logic_error("subscription: admission closed");
    if (size_ == slots_.size()) throw std::length_error("subscription: queue full");
    auto owned = store_->retain(source);
    append_owned(owned);
}

// 将预先准备的事件引用转交已检查的队尾，提交阶段不再产生失败点。
void SubscriptionQueue::append_owned(runtime::Handle& owned) noexcept {
    slots_[tail_] = std::exchange(owned, {});
    tail_ = (tail_ + 1) % slots_.size();
    ++size_;
}

// 将队首数据转交消费者，排空最后一项后流结束，但外部数据引用仍有效。
// 步骤：区分空队列状态 → 移出队首引用 → 按关闭模式更新队列 → 转交数据持有。
QueueRead SubscriptionQueue::poll() {
    if (!size_) return {state_ == QueueState::open ? PollState::empty : PollState::closed, {}};
    const auto owned = std::exchange(slots_[head_], {});
    head_ = (head_ + 1) % slots_.size();
    --size_;
    if (!size_ && state_ == QueueState::draining) state_ = QueueState::closed;
    return {PollState::event, EventLease(store_, owned)};
}

// 关闭接纳，排空保留既有项，取消仅回收尚在队列中的引用。
// 步骤：校验模式并复用终态 → 固定关闭方式 → 按需丢弃排队引用 → 判断流是否结束。
void SubscriptionQueue::close(CloseMode mode) {
    if (mode != CloseMode::drain && mode != CloseMode::cancel) throw std::invalid_argument("subscription: invalid close mode");
    if (state_ == QueueState::closed) return;
    mode_ = mode;
    state_ = QueueState::draining;
    if (mode == CloseMode::cancel) {
        while (size_) { auto item = poll(); item.event.reset(); }
    }
    if (!size_) state_ = QueueState::closed;
}
}
