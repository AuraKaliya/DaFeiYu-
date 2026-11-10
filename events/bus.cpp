#include <dafeiyu/events.hpp>
#include <limits>
#include <stdexcept>
#include <utility>

namespace dfy::events {
// 保存回调和所有者，仅在注册成功后推进订阅标识。
// 步骤：校验参数和标识余量 → 追加稳定的订阅节点 → 返回标识。
SubscriptionId Bus::subscribe(OwnerId owner, Callback callback) {
    if (owner == 0 || !callback) {
        throw std::invalid_argument("events: expected nonzero owner and nonempty callback");
    }
    if (next_id_ == std::numeric_limits<SubscriptionId>::max()) {
        throw std::overflow_error("events: subscription identifiers exhausted");
    }

    subscriptions_.push_back({next_id_, owner, std::move(callback)});
    return next_id_++;
}

// 使指定订阅立即失效，未知或已取消的标识不产生变更。
// 步骤：查找有效订阅 → 标记取消 → 在可释放时回收回调。
bool Bus::unsubscribe(SubscriptionId id) noexcept {
    for (auto& subscription : subscriptions_) {
        if (subscription.id == id && subscription.active) {
            subscription.active = false;
            collect();
            return true;
        }
    }
    return false;
}

// 撤销同一所有者的全部订阅，支持在通知期间集中取消。
// 步骤：标记匹配的有效订阅并计数 → 在可释放时回收回调。
std::size_t Bus::unsubscribe_owner(OwnerId owner) noexcept {
    std::size_t count = 0;
    for (auto& subscription : subscriptions_) {
        if (subscription.owner == owner && subscription.active) {
            subscription.active = false;
            ++count;
        }
    }

    collect();
    return count;
}

// 发布固定边界内仍有效的订阅，单个回调失败不阻断其余通知。
// 步骤：拒绝重入并固定订阅边界 → 顺序通知并汇总异常 → 结束分发并回收取消项。
PublishResult Bus::publish(Event event) noexcept {
    if (publishing_) {
        return {PublishStatus::reentrant};
    }
    publishing_ = true;
    const auto boundary = next_id_;
    PublishResult result;

    for (auto& subscription : subscriptions_) {
        if (subscription.id >= boundary) {
            break;
        }
        if (!subscription.active) {
            continue;
        }
        try {
            subscription.callback(event);
            ++result.succeeded;
        } catch (...) {
            // 固定大小的诊断不分配内存，也不保留可能来自 DLL 的异常对象。
            if (result.failed++ == 0) {
                result.first_failed_subscription = subscription.id;
                result.first_failed_owner = subscription.owner;
            }
        }
    }

    publishing_ = false;
    collect();
    return result;
}

// 分发结束后移除失效节点，保持正在执行的回调和迭代位置稳定。
void Bus::collect() noexcept {
    if (!publishing_) {
        subscriptions_.remove_if([](const auto& subscription) { return !subscription.active; });
    }
}
}
