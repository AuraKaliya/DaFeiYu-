#include <dafeiyu/group_reader.hpp>
#include <iostream>
namespace {
using namespace dfy::runtime;
using namespace dfy::events;
// 在成员读取、取消或回收边界违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 验证预期异常类别，保留无关失败供入口报告。
template<class Error, class Action>
void rejects(Action action) { try { action(); } catch (const Error&) { return; } throw std::runtime_error("group read accepted invalid action"); }
// 创建空正文的合法事件，测试聚焦成员领取与结果持有。
std::shared_ptr<const StructValue> payload() {
    auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.group_read",1,{}, {0,32}));
    return std::make_shared<const StructValue>(StructValue::create(type,{},0));
}
// 验证取消不领取事件，成员退出不结束其他成员，终态持有仍占容量。
// 步骤：建立两个成员等待 → 取消先到并由另一成员领取 → 独立关闭 → 验证外部撤销及槽位复用。
void isolation() {
    auto store = std::make_shared<EventStore>(4,0); auto group = std::make_shared<ConsumerGroup>(store,2,2);
    GroupReader first(group,1), second(group,1);
    auto cancelled = first.next();
    rejects<std::logic_error>([&] { (void)first.next(); });
    cancelled->request_cancel(CancelReason::user); const auto id = group->publish(payload()); first.advance();
    check(cancelled->result().outcome == ReadOutcome::cancelled && group->queued() == 1 && group->member_count() == 2,"cancel consumed event or left group");
    auto received = second.next(); check(received->event().id == id && !group->queued(),"second member did not receive event");
    check(received->request_cancel(CancelReason::deadline) == CancelDisposition::already_settled,"late cancel changed delivery");
    rejects<std::length_error>([&] { (void)first.next(); });
    cancelled.reset(); first.collect(); auto closed = first.next(); first.close(); first.close();
    check(closed->result().outcome == ReadOutcome::closed && closed->result().close_mode == CloseMode::cancel &&
        group->member_count() == 1 && group->state() == QueueState::open,"member close ended shared group");
    second.close(); check(received->event().id == id && !group->member_count() && store->references() == 1,"member exit revoked delivered data");
    received.reset(); second.collect(); check(!store->references(),"group read result leaked");

    auto revoked = std::make_unique<GroupReader>(group,1); auto pending = revoked->next();
    const auto old = revoked->member(); group->leave(old); const auto replacement = group->join();
    const auto queued = group->publish(payload()); revoked->advance();
    check(pending->result().outcome == ReadOutcome::closed && pending->result().close_mode == CloseMode::cancel && group->queued() == 1,"revoked member consumed queued data");
    revoked.reset(); check(group->has_member(replacement) && group->poll(replacement).event.get().id == queued,"revoked reader removed replacement member");
    group->leave(replacement); check(!store->references(),"revoked member leaked delivery");
}
// 验证排空结束与析构取消，数据可跨控制器和组寿命存活。
// 步骤：排空最后事件并退出成员 → 保留结果后释放控制器 → 验证无客户端 Pending 与析构取消 → 拒绝非法构造。
void lifetime() {
    auto store = std::make_shared<EventStore>(4,0); auto group = std::make_shared<ConsumerGroup>(store,2,2);
    auto reader = std::make_unique<GroupReader>(group,2); auto request = reader->next();
    const auto id = group->publish(payload()); group->close(CloseMode::drain); reader->advance();
    check(request->event().id == id && reader->queue_state() == QueueState::closed && reader->close_mode() == CloseMode::drain && !group->member_count(),"draining member did not release registration");
    auto ended = reader->next(); reader->close();
    check(ended->result().outcome == ReadOutcome::closed && ended->result().close_mode == CloseMode::drain,"member close rewrote group end reason");
    const std::weak_ptr<ConsumerGroup> weak = group; group.reset(); reader.reset();
    check(weak.expired() && request->event().id == id && store->references() == 1,"result kept group or lost data");
    request.reset(); ended.reset(); check(!store->references(),"drained group result leaked");

    group = std::make_shared<ConsumerGroup>(store,1,1);
    std::shared_ptr<ReadRequest> abandoned;
    {
        GroupReader owner(group,1); abandoned = owner.next();
        check(group->member_count() == 1 && abandoned->operation()->state() == OperationState::pending,"group pending state wrong");
    }
    check(abandoned->result().outcome == ReadOutcome::cancelled && abandoned->operation()->cancellation_reason() == CancelReason::owner_stop &&
        !group->member_count() && group->state() == QueueState::open,"reader destructor ended shared group or lost pending");
    {
        GroupReader owner(group,1); auto client = owner.next(); const auto operation = client->operation(); client.reset(); owner.collect();
        check(owner.size() == 1 && operation->state() == OperationState::pending,"client release cancelled pending read");
        owner.close(); owner.collect(); check(!owner.size() && operation->state() == OperationState::succeeded,"member close did not settle retained wait");
    }
    rejects<std::invalid_argument>([&] { GroupReader invalid({},1); });
    rejects<std::invalid_argument>([&] { GroupReader invalid(group,0); });
    check(!group->member_count(),"failed reader constructor joined group");
}
}
// 验收组成员异步读取的单次领取、独立退出及完整结果寿命。
// 步骤：核对取消和成员隔离 → 核对排空与析构 → 报告结果。
int main() {
    try { isolation(); lifetime(); std::cout << "Group member read checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
