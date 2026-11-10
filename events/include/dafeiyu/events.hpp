#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>

namespace dfy::events {
using OwnerId = std::uint64_t;
using SubscriptionId = std::uint64_t;
enum class Kind { turn_started, turn_completed, turn_failed, session_cleared };
struct Event {
    Kind kind;
    std::uint64_t session_id;
    std::uint64_t turn_id;
};
enum class PublishStatus { completed, reentrant };
struct PublishResult {
    PublishStatus status = PublishStatus::completed;
    std::size_t succeeded = 0;
    std::size_t failed = 0;
    SubscriptionId first_failed_subscription = 0;
    OwnerId first_failed_owner = 0;
};

// 单线程同步通知；调用方保证总线活到发布结束，回调对象析构不得重入总线。
class Bus {
  public:
    using Callback = std::function<void(const Event&)>;

    // 创建独立的空总线，订阅标识仅在本实例内有效。
    Bus() = default;
    // 销毁剩余订阅及回调，调用方须保证捕获资源的生命周期。
    ~Bus() = default;
    // 禁止复制，避免复制订阅归属和正在执行的分发状态。
    Bus(const Bus&) = delete;
    // 禁止赋值，保持订阅标识与总线实例的对应关系。
    Bus& operator=(const Bus&) = delete;

    // 注册非空回调与非零所有者，返回不复用的标识；非法参数或分配失败时抛出异常。
    SubscriptionId subscribe(OwnerId owner, Callback callback);
    // 取消仍有效的订阅并返回是否成功；分发中的回调对象延至本次发布结束释放。
    bool unsubscribe(SubscriptionId id) noexcept;
    // 取消所有者的全部有效订阅，返回取消数量；不影响其他所有者。
    std::size_t unsubscribe_owner(OwnerId owner) noexcept;
    // 按注册顺序通知，事件引用仅在回调内有效；拒绝同一总线嵌套发布并汇总回调异常。
    PublishResult publish(Event event) noexcept;

  private:
    struct Subscription {
        SubscriptionId id;
        OwnerId owner;
        Callback callback;
        bool active = true;
    };
    // 仅在没有分发时销毁已取消的回调，避免自取消破坏当前调用。
    void collect() noexcept;

    std::list<Subscription> subscriptions_;
    SubscriptionId next_id_ = 1;
    bool publishing_ = false;
};
}
