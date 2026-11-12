#pragma once
#include <dafeiyu/event_store.hpp>
#include <optional>

namespace dfy::events {
class SubscriptionQueue;
class BroadcastChannel;

// 移动式事件数据持有，只管理数据寿命，不代表处理成功或消费者任务退出。
class EventLease {
  public:
    // 创建不包含事件的空持有。
    EventLease() = default;
    // 归还事件引用，最后持有者回收宿主事件数据。
    ~EventLease() { reset(); }
    // 禁止隐式复制，独立持有通过 retain 显式取得。
    EventLease(const EventLease&) = delete;
    // 禁止复制赋值覆盖已有事件持有。
    EventLease& operator=(const EventLease&) = delete;
    // 移动转交数据引用并清空来源标识。
    EventLease(EventLease&& other) noexcept : store_(std::move(other.store_)), owned_(std::exchange(other.owned_, {})) {}
    // 归还旧引用后接收新数据持有，自移动无操作。
    EventLease& operator=(EventLease&& other) noexcept;
    // 借用事件记录，空持有拒绝，借用仅在本份持有有效期间使用。
    const EventRecord& get() const;
    // 取得同一事件的独立持有，受存储引用容量约束。
    EventLease retain() const;
    // 归还本份事件引用，空持有重复归还无操作。
    void reset() noexcept;
    // 判断是否包含有效事件数据。
    explicit operator bool() const noexcept { return !owned_.empty(); }
  private:
    friend class SubscriptionQueue;
    // 接收已转交的引用并保持签发存储寿命，不再次增加事件引用。
    EventLease(std::shared_ptr<EventStore> store, runtime::Handle owned) noexcept : store_(std::move(store)), owned_(owned) {}
    std::shared_ptr<EventStore> store_;
    runtime::Handle owned_;
};

enum class QueueState { open, draining, closed };
enum class CloseMode { drain, cancel };
enum class PollState { empty, event, closed };
struct QueueRead { PollState state; EventLease event; };

// 同一宿主线程内的有界独立接收队列，队列和读出引用分别保持事件存储。
class SubscriptionQueue {
  public:
    // 建立正容量队列并保持非空事件存储，失败不接纳事件。
    SubscriptionQueue(std::shared_ptr<EventStore> store, std::size_t capacity);
    // 取消剩余交付并释放队列引用，不撤销已读出的事件或取消业务工作。
    ~SubscriptionQueue() { close(CloseMode::cancel); }
    // 禁止复制接收队列及其引用责任。
    SubscriptionQueue(const SubscriptionQueue&) = delete;
    // 禁止赋值或隐式移动接收队列。
    SubscriptionQueue& operator=(const SubscriptionQueue&) = delete;
    // 显式保留来源事件入队，拒绝时不消耗来源引用或改动队列。
    void push(runtime::Handle source);
    // 转交队首引用，分别返回事件、暂时为空或流结束，不代表处理任务退出。
    QueueRead poll();
    // 停止接纳并选择排空或取消；排空可升级取消，已关闭状态不再改写。
    void close(CloseMode mode);
    // 返回队列状态，Closed 仅表示本交付队列已经结束。
    QueueState state() const noexcept { return state_; }
    // 返回已选择的关闭方式，Open 时没有关闭原因。
    std::optional<CloseMode> close_mode() const noexcept { return state_ == QueueState::open ? std::nullopt : std::optional(mode_); }
    // 返回排队事件数量，不包含已经交给消费者的数据引用。
    std::size_t size() const noexcept { return size_; }
    // 返回当前空槽数量，关闭状态下也不允许利用空槽接纳新事件。
    std::size_t available() const noexcept { return slots_.size() - size_; }
  private:
    friend class BroadcastChannel;
    // 接收已准备且属于本存储的引用；仅在已确认 Open 且有空槽时使用，不分配内存。
    void append_owned(runtime::Handle& owned) noexcept;
    std::shared_ptr<EventStore> store_;
    std::vector<runtime::Handle> slots_;
    std::size_t head_ = 0, tail_ = 0, size_ = 0;
    QueueState state_ = QueueState::open;
    CloseMode mode_ = CloseMode::drain;
};
}
