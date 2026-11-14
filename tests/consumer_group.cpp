#include <dafeiyu/consumer_group.hpp>
#include <dafeiyu/broadcast_channel.hpp>
#include <iostream>
namespace {
using namespace dfy::events;
using namespace dfy::runtime;
// 在成员隔离、单次领取或关闭责任违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 验证预期异常类别，保留无关失败供入口定位。
template<class Error, class Action>
void rejects(Action action) { try { action(); } catch (const Error&) { return; } throw std::runtime_error("consumer group accepted invalid action"); }
// 创建没有正文的合法事件，验收重点为组交付与资源计量。
std::shared_ptr<const StructValue> payload() {
    const auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.group",1,{}, {0,32}));
    return std::make_shared<const StructValue>(StructValue::create(type,{},0));
}
// 验证共享队列、成员代次、独立退出及已领取数据跨组寿命。
// 步骤：无成员接纳 → 两成员单次领取 → 撤销并替换成员 → 验证存储预算 → 销毁组后释放数据。
void members() {
    auto store = std::make_shared<EventStore>(2,0);
    EventLease second_data, third_data;
    {
        ConsumerGroup group(store,2,1);
        const auto first_id = group.publish(payload());
        check(group.queued() == 1 && !group.member_count() && store->references() == 1,"group admission depended on member presence");
        rejects<std::length_error>([&] { (void)group.publish(payload()); });
        const auto first = group.join(), second = group.join();
        rejects<ReferenceError>([&] { (void)group.join(); });
        auto received = group.poll(first);
        check(received.state == PollState::event && received.event.get().id == first_id && group.poll(second).state == PollState::empty,
            "group broadcast one delivery to two members");
        group.leave(first);
        check(group.member_count() == 1 && received.event.get().id == first_id,"leaving member invalidated delivered data");
        const auto replacement = group.join();
        check(replacement != first && group.member_count() == 2,"group reused revoked member identity");
        rejects<ReferenceError>([&] { (void)group.poll(first); });
        rejects<ReferenceError>([&] { group.leave(first); });
        rejects<ReferenceError>([&] { group.leave({}); });
        ConsumerGroup other(store,1,1); const auto foreign = other.join();
        rejects<ReferenceError>([&] { (void)group.poll(foreign); });
        rejects<ReferenceError>([&] { group.leave(foreign); });

        const auto second_id = group.publish(payload()); second_data = std::move(group.poll(second).event);
        check(second_id.sequence == first_id.sequence + 1 && store->references() == 2,"group queue refusal consumed sequence");
        rejects<ReferenceError>([&] { (void)group.publish(payload()); });
        check(!group.queued(),"group storage failure queued partial event");
        received.event.reset();
        const auto third_id = group.publish(payload()); third_data = std::move(group.poll(replacement).event);
        check(third_id.sequence == second_id.sequence + 1,"group storage refusal consumed sequence");
        group.leave(second); group.close(CloseMode::drain);
        check(group.state() == QueueState::closed && group.poll(replacement).state == PollState::closed && group.close_mode() == CloseMode::drain,
            "group empty drain did not close");
        rejects<std::logic_error>([&] { (void)group.join(); });
        rejects<std::logic_error>([&] { (void)group.publish(payload()); });
    }
    check(second_data.get().id.sequence == 2 && third_data.get().id.sequence == 3 && store->references() == 2,"group exit revoked claimed data");
    second_data.reset(); third_data.reset(); check(!store->references(),"group claimed data leaked");
}
// 验证排空与取消的独立语义，成员退出不隐式关闭共享交付。
// 步骤：排空两项交付 → 验证零成员仍保留待领事件 → 升级取消 → 共享存储下与广播各自接纳。
void closure() {
    auto store = std::make_shared<EventStore>(4,0);
    ConsumerGroup group(store,2,2); const auto first = group.join(), second = group.join();
    const auto first_id = group.publish(payload()), second_id = group.publish(payload());
    group.close(CloseMode::drain);
    check(group.state() == QueueState::draining && group.queued() == 2,"group drain discarded data");
    rejects<std::logic_error>([&] { (void)group.join(); });
    auto first_data = group.poll(first); group.leave(first);
    check(group.state() == QueueState::draining && group.member_count() == 1 && first_data.event.get().id == first_id,"member exit ended sibling drain");
    check(group.poll(second).event.get().id == second_id && group.state() == QueueState::closed,"group drain order changed");
    first_data.event.reset(); group.leave(second);

    ConsumerGroup waiting(store,1,1); const auto member = waiting.join();
    const auto queued = waiting.publish(payload()); waiting.leave(member);
    check(waiting.queued() == 1 && !waiting.member_count() && waiting.state() == QueueState::open,"last member discarded queued delivery");
    waiting.close(CloseMode::drain); waiting.close(CloseMode::cancel);
    check(waiting.state() == QueueState::closed && waiting.close_mode() == CloseMode::cancel && !store->references(),"group drain could not upgrade to cancel");
    ConsumerGroup fresh(store,1,1); const auto receiver = fresh.join();
    BroadcastChannel broadcast(store,1,1); const auto subscriber = broadcast.subscribe();
    const auto directed = fresh.publish(payload()), copied = broadcast.publish(payload());
    check(directed.sequence == queued.sequence + 1 && copied.sequence == directed.sequence + 1 &&
        fresh.poll(receiver).event.get().id == directed && subscriber->poll().event.get().id == copied,"group and broadcast storage identities diverged");
    check(!store->references(),"group closure leaked events");
    rejects<std::invalid_argument>([&] { (void)fresh.publish({}); });
    rejects<std::invalid_argument>([&] { ConsumerGroup invalid({},1,1); });
    rejects<std::invalid_argument>([&] { ConsumerGroup invalid(store,0,1); });
    rejects<std::invalid_argument>([&] { ConsumerGroup invalid(store,1,0); });
}
// 验证同一事件原子交付给独立订阅和消费者组，组内只由一个成员领取。
// 步骤：混合广播 → 检查慢组背压 → 指定组交付 → 验证目录归属与控制持有 → 关闭通道保留已交付数据。
void integrated_delivery() {
    auto store = std::make_shared<EventStore>(3,0);
    BroadcastChannel channel(store,3,1);
    const auto subscriber = channel.subscribe();
    const auto group = channel.subscribe_group(2), slow = channel.subscribe_group(1);
    const auto first = group->join(), second = group->join();
    rejects<std::length_error>([&] { (void)channel.subscribe_group(1); });
    rejects<std::invalid_argument>([&] { (void)channel.subscribe_group(0); });
    const auto original = channel.publish(payload());
    auto observed = subscriber->poll(), claimed = group->poll(first);
    check(channel.size() == 3 && store->references() == 3 && observed.event.get().id == original && claimed.event.get().id == original &&
        group->poll(second).state == PollState::empty && slow->queued() == 1,"broadcast group duplicated delivery per member");
    rejects<std::length_error>([&] { (void)channel.publish(payload()); });
    check(!subscriber->size() && !group->queued() && slow->queued() == 1,"slow group caused partial broadcast");
    observed.event.reset(); claimed.event.reset();
    const auto directed = channel.publish_to_group(payload(),group);
    auto targeted = group->poll(second);
    check(directed.sequence == original.sequence + 1 && targeted.event.get().id == directed && !subscriber->size() && slow->queued() == 1,
        "group-targeted event reached independent subscriber");
    const auto late_member = slow->join();
    check(slow->poll(late_member).event.get().id == original,"late group member lost accepted delivery");
    rejects<ReferenceError>([&] { (void)channel.publish(payload()); });
    check(!subscriber->size() && !group->queued() && !slow->queued(),"group retained data bypassed global reference budget");
    targeted.event.reset();
    const auto next = channel.publish(payload());
    check(next.sequence == directed.sequence + 1 && subscriber->poll().event.get().id == next && group->poll(first).event.get().id == next &&
        slow->poll(late_member).event.get().id == next,"mixed target retry changed event identity");
    check(!store->references(),"mixed delivery leaked data");

    const auto foreign = std::make_shared<ConsumerGroup>(store,1,1);
    rejects<std::invalid_argument>([&] { (void)channel.publish_to_group(payload(),foreign); });
    rejects<std::invalid_argument>([&] { channel.remove_group(foreign); });
    rejects<std::invalid_argument>([&] { (void)channel.publish_to_group(payload(),{}); });
    rejects<std::logic_error>([&] { channel.remove_group(group); });
    group->close(CloseMode::drain);
    rejects<std::logic_error>([&] { (void)channel.publish_to_group(payload(),group); });
    channel.remove_group(group);
    rejects<std::invalid_argument>([&] { (void)channel.publish_to_group(payload(),group); });
    auto replacement = channel.subscribe_group(1);
    const std::weak_ptr<ConsumerGroup> lifetime = replacement;
    replacement.reset(); check(!lifetime.expired(),"channel did not keep group controller");
    {
        const auto owned = lifetime.lock(); owned->close(CloseMode::cancel); channel.remove_group(owned);
    }
    check(lifetime.expired() && channel.size() == 2,"removed group control retained");

    std::shared_ptr<ConsumerGroup> outside; EventLease data; MemberId member{};
    {
        BroadcastChannel temporary(store,1,1); outside = temporary.subscribe_group(1); member = outside->join();
        const auto id = temporary.publish(payload()); data = std::move(outside->poll(member).event);
        check(data.get().id == id,"temporary group received wrong identity");
    }
    check(outside->state() == QueueState::closed && outside->poll(member).state == PollState::closed && data && store->references() == 1,
        "channel exit invalidated group data or left group open");
    data.reset(); check(!store->references(),"group data not reclaimed after channel exit");
}
}
// 验收有界成员与交付层单次领取，不将数据交出或释放当作业务成功。
int main() {
    try { members(); closure(); integrated_delivery(); std::cout << "Consumer group membership checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
