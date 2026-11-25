#include <dafeiyu/broadcast_channel.hpp>
#include <iostream>

namespace {
using namespace dfy::events;
using namespace dfy::runtime;
// 在广播出现部分接纳或接收方互相干扰时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 验证指定拒绝类别，避免将无关错误视为预期失败。
template<class Error, class Action>
void rejects(Action action) { try { action(); } catch (const Error&) { return; } throw std::runtime_error("targeted delivery accepted invalid input"); }
// 创建合法不可变事件载荷，广播测试聚焦交付关系。
std::shared_ptr<const StructValue> payload() {
    auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("app.event", 1, {}, {0, 32}));
    return std::make_shared<const StructValue>(StructValue::create(type, {}, 0));
}

// 验证队列及已交付数据背压均原子拒绝，不让快接收方得到半次广播。
// 步骤：独立广播 → 留下慢接收方 → 验证两层拒绝 → 归还引用并再次接纳。
void verify_atomic_delivery() {
    auto store = std::make_shared<EventStore>(2, 0);
    BroadcastChannel channel(store, 2, 1);
    const auto first = channel.subscribe(), second = channel.subscribe();
    const auto id = channel.publish(payload());
    auto fast = first->poll();
    bool queue_full = false;
    try { (void)channel.publish(payload()); } catch (const std::length_error&) { queue_full = true; }
    check(queue_full && first->poll().state == PollState::empty && second->size() == 1 && store->references() == 2,
        "slow receiver caused partial broadcast");
    auto slow = second->poll();
    check(fast.event.get().id == id && slow.event.get().id == id, "broadcast created distinct event identities");
    bool references_full = false;
    try { (void)channel.publish(payload()); } catch (const ReferenceError&) { references_full = true; }
    check(references_full && !first->size() && !second->size(), "retained data bypassed reference budget");
    fast.event.reset();
    slow.event.reset();
    const auto next = channel.publish(payload());
    check(next.sequence == id.sequence + 1 && first->size() == 1 && second->size() == 1, "rejected broadcast consumed sequence");
    first->close(CloseMode::cancel);
    channel.remove(first);
    auto received = second->poll();
    check(received.event.get().id == next && first->poll().state == PollState::closed, "receiver close affected sibling");
    received.event.reset();
    const auto replacement = channel.subscribe();
    check(replacement != first && channel.size() == 2, "removed subscription reused object identity");
    const auto third = channel.publish(payload());
    check(second->poll().event.get().id == third && replacement->poll().event.get().id == third, "replacement lost broadcast");
}

// 验证批量引用不足无半批、无接收方接纳以及通道销毁后的数据和结束状态。
// 步骤：拒绝超预算广播 → 移除一个接收方后重试 → 转交数据 → 销毁通道并最终释放。
void verify_bounds_and_lifetime() {
    auto store = std::make_shared<EventStore>(2, 0);
    EventLease retained;
    std::shared_ptr<SubscriptionQueue> outside;
    {
        BroadcastChannel channel(store, 3, 1);
        const auto empty_id = channel.publish(payload());
        check(empty_id.sequence == 1 && !store->references(), "no-receiver admission leaked");
        outside = channel.subscribe();
        const auto second = channel.subscribe(), third = channel.subscribe();
        bool full = false, subscription_full = false, not_closed = false, foreign = false;
        try { (void)channel.subscribe(); } catch (const std::length_error&) { subscription_full = true; }
        try { (void)channel.publish(payload()); } catch (const ReferenceError&) { full = true; }
        try { channel.remove(second); } catch (const std::logic_error&) { not_closed = true; }
        try { channel.remove(std::make_shared<SubscriptionQueue>(store, 1)); } catch (const std::invalid_argument&) { foreign = true; }
        check(full && subscription_full && not_closed && foreign && !outside->size() && !second->size() && !store->references(),
            "failed broadcast or removal changed receivers");
        third->close(CloseMode::drain);
        channel.remove(third);
        const auto id = channel.publish(payload());
        check(id.sequence == 2 && store->references() == 2, "batch admission or rollback failed");
        retained = std::move(outside->poll().event);
    }
    check(outside->poll().state == PollState::closed && retained.get().id.sequence == 2 && store->references() == 1,
        "channel exit lost data or left receiver open");
    retained.reset();
    check(!store->references(), "broadcast leaked event reference");
}
// 验证目标集合的唯一性、归属、关闭及背压隔离，拒绝时不消耗事件序号。
// 步骤：填满非目标队列 → 定向交付并制造目标背压 → 拒绝错误目标 → 移除及替换后再次交付。
void verify_targeted_delivery() {
    auto store = std::make_shared<EventStore>(8,0);
    BroadcastChannel channel(store,3,1);
    const auto first = channel.subscribe(), second = channel.subscribe(), third = channel.subscribe();
    const auto occupied = channel.publish_to(payload(),{&third,1});
    const std::shared_ptr<SubscriptionQueue> targets[] = {second,first};
    const auto directed = channel.publish_to(payload(),targets);
    check(directed.sequence == occupied.sequence + 1 && first->size() == 1 && second->size() == 1 && third->size() == 1,
        "non-target backpressure blocked directed delivery");
    auto retained = first->poll();
    rejects<std::length_error>([&] { (void)channel.publish_to(payload(),targets); });
    check(!first->size() && second->size() == 1 && store->references() == 3,"full target caused partial delivery");
    const auto next = channel.publish_to(payload(),{&first,1});
    check(next.sequence == directed.sequence + 1 && retained.event.get().id == directed,"target rejection consumed sequence or changed old data");
    check(first->poll().event.get().id == next && second->poll().event.get().id == directed,"target delivery changed per-receiver order");

    rejects<std::invalid_argument>([&] { (void)channel.publish_to(payload(),{}); });
    const std::shared_ptr<SubscriptionQueue> empty;
    rejects<std::invalid_argument>([&] { (void)channel.publish_to(payload(),{&empty,1}); });
    const std::shared_ptr<SubscriptionQueue> duplicates[] = {second,second};
    rejects<std::invalid_argument>([&] { (void)channel.publish_to(payload(),duplicates); });
    BroadcastChannel other(store,1,1); const auto foreign = other.subscribe();
    const std::shared_ptr<SubscriptionQueue> mixed[] = {second,foreign};
    rejects<std::invalid_argument>([&] { (void)channel.publish_to(payload(),mixed); });
    const std::shared_ptr<SubscriptionQueue> excessive[] = {first,second,third,foreign};
    rejects<std::length_error>([&] { (void)channel.publish_to(payload(),excessive); });
    rejects<std::invalid_argument>([&] { (void)channel.publish_to({},targets); });
    check(!first->size() && !second->size() && third->size() == 1,"invalid target request changed queues");
    third->close(CloseMode::drain);
    rejects<std::logic_error>([&] { (void)channel.publish_to(payload(),{&third,1}); });
    check(third->poll().event.get().id == occupied && third->state() == QueueState::closed,"target rejection changed draining queue");
    rejects<std::logic_error>([&] { (void)channel.publish_to(payload(),{&third,1}); });
    channel.remove(third);
    const auto replacement = channel.subscribe();
    rejects<std::invalid_argument>([&] { (void)channel.publish_to(payload(),{&third,1}); });
    const auto final = channel.publish_to(payload(),{&replacement,1});
    check(final.sequence == next.sequence + 1 && !first->size() && !second->size() && replacement->poll().event.get().id == final,
        "removed target reached replacement or invalid request consumed sequence");
    retained.event.reset(); check(!store->references(),"targeted data references leaked");
}
// 验证共享存储预算不足时目标集合原子拒绝，普通广播继续共享同一接纳序号。
// 步骤：拒绝超额目标引用 → 留存已读数据并再次拒绝 → 归还后交付 → 混合普通广播。
void verify_targeted_budget() {
    auto store = std::make_shared<EventStore>(1,0);
    BroadcastChannel channel(store,2,1);
    const auto first = channel.subscribe(), second = channel.subscribe();
    const std::shared_ptr<SubscriptionQueue> both[] = {first,second};
    rejects<ReferenceError>([&] { (void)channel.publish_to(payload(),both); });
    check(!first->size() && !second->size() && !store->references(),"target reference failure partially committed");
    const auto initial = channel.publish_to(payload(),{&first,1}); auto held = first->poll();
    rejects<ReferenceError>([&] { (void)channel.publish_to(payload(),{&second,1}); });
    check(initial.sequence == 1 && !second->size(),"target reference failure consumed sequence");
    held.event.reset();
    const auto next = channel.publish_to(payload(),{&second,1});
    check(next.sequence == 2 && second->poll().event.get().id == next,"target retry changed identity");
    first->close(CloseMode::cancel); channel.remove(first);
    const auto broadcast = channel.publish(payload());
    check(broadcast.sequence == 3 && second->poll().event.get().id == broadcast,"targeted and broadcast paths diverged");
    check(!store->references(),"targeted references leaked after delivery");
}
}

// 验收独立广播的原子接纳和有界持有，不将接纳回执当作业务结果。
// 步骤：验证原子交付 → 验证容量及寿命 → 汇报结果。
int main() {
    try {
        verify_atomic_delivery();
        verify_bounds_and_lifetime();
        verify_targeted_delivery();
        verify_targeted_budget();
        std::cout << "Broadcast channel checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
