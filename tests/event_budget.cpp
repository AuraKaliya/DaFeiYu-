#include <dafeiyu/processing_queue.hpp>
#include <iostream>
namespace {
using namespace dfy::runtime;
using namespace dfy::events;
// 在共享存储和所有者计费边界违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 验证容量拒绝，不吞没其他业务异常。
template<class Action>
void exhausted(Action action) { try { action(); } catch (const std::length_error&) { return; } throw std::runtime_error("event budget allowed excess"); }
// 创建含八字节整数的不可变事件载荷。
std::shared_ptr<const StructValue> payload() {
    const FieldDescriptor field{"id",ValueKind::uint64,0};
    const auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.budget",1,{&field,1},{1,32}));
    const NamedValue value{"id",Value::from_uint64(42)};
    return std::make_shared<const StructValue>(StructValue::create(type,{&value,1},8));
}
// 验证同一事件多份交付只计一次载荷，最后持有释放前存储预算不归还。
// 步骤：批量接纳同一事件 → 拒绝字节超额 → 逐份释放 → 再接纳并验证连续序号。
void shared_storage() {
    auto global = std::make_shared<Budget>(BudgetAmount{4,8});
    EventStore store(5,8,global); auto data = payload(); const auto batch = store.accept_batch(data,3);
    check(global->used().items == 1 && global->used().bytes == 8 && store.references() == 3,"shared payload counted per delivery");
    exhausted([&] { (void)store.accept(data); });
    store.release(batch[0]); store.release(batch[1]);
    check(global->used().bytes == 8 && store.references() == 1,"partial release returned live payload budget");
    store.release(batch[2]); check(!global->used().items && !global->used().bytes,"last event hold did not return budget");
    const auto next = store.accept(data);
    check(store.get(next).id.sequence == 2,"budget rejection consumed event sequence");
    store.release(next);
}
// 验证跨队列的所有者上限，处理退出和普通事件持有分别归还各自的计量。
// 步骤：建立共享所有者与独立队列 → 拒绝超量且保留顺序 → 退出但保留结果 → 回收后恢复接纳。
void processing_owners() {
    auto memory = std::make_shared<Budget>(BudgetAmount{4,8});
    auto store = std::make_shared<EventStore>(10,8,memory); SubscriptionQueue receiver(store,1);
    const auto handle = store->accept(payload()); receiver.push(handle); store->release(handle); auto data = receiver.poll().event;
    auto host = std::make_shared<Budget>(BudgetAmount{4,24}); auto owner = std::make_shared<Budget>(BudgetAmount{2,16},host);
    ProcessingQueue first(3,true,owner), second(3,true,owner), independent(2,true,host);
    auto a = first.submit(data), b = second.submit(data);
    exhausted([&] { (void)first.submit(data); });
    auto c = independent.submit(data);
    exhausted([&] { (void)independent.submit(data); });
    check(owner->used().items == 2 && host->used().bytes == 24 && memory->used().bytes == 8,"owner and actual payload accounting mixed");
    first.advance(); first.start(a); first.complete(a,OperationState::succeeded); first.finish(a); first.collect();
    exhausted([&] { (void)first.submit(data); });
    auto retained = a->retain_event(); a.reset(); first.collect();
    auto replacement = first.submit(data);
    check(replacement->ordering()->sequence() == 2 && owner->used().items == 2,"failed processing admission consumed serial identity");
    first.close(CloseMode::cancel); second.close(CloseMode::cancel); independent.close(CloseMode::cancel);
    replacement.reset(); b.reset(); c.reset(); first.collect(); second.collect(); independent.collect();
    check(!host->used().items && !owner->used().bytes && memory->used().bytes == 8,"processing exit erased retained event charge");
    data.reset(); check(memory->used().bytes == 8,"independent retained event lost budget");
    retained.reset(); check(!memory->used().bytes && !memory->used().items,"storage budget survived final event lease");
}
// 验证最后数据持有的实际释放阶段仍计费，不能先归还额度再销毁载荷。
// 步骤：建立可观察析构的载荷 → 转入处理并释放外部持有 → 取消和回收处理 → 核对释放期间及之后的计量。
void release_boundary() {
    auto memory = std::make_shared<Budget>(BudgetAmount{1,8}), owner = std::make_shared<Budget>(BudgetAmount{1,8});
    bool charged_during_release = false;
    auto tracked = std::shared_ptr<const StructValue>(new StructValue(*payload()),[&](const StructValue* value) {
        charged_during_release = memory->used().items == 1 && owner->used().items == 1;
        delete value;
    });
    auto store = std::make_shared<EventStore>(4,8,memory); SubscriptionQueue incoming(store,1);
    const auto handle = store->accept(tracked); tracked.reset(); incoming.push(handle); store->release(handle); auto data = incoming.poll().event;
    ProcessingQueue processing(1,true,owner); auto record = processing.submit(data); data.reset();
    processing.close(CloseMode::cancel); record.reset(); processing.collect();
    check(charged_during_release && !memory->used().items && !owner->used().items,"quota returned before payload ownership was released");
}
}
// 验收真实存储与处理的分层预算，区分共享存储成本和每份处理持有关系。
// 步骤：验证事件共享计费 → 验证所有者汇总与退出寿命 → 报告结果。
int main() {
    try { shared_storage(); processing_owners(); release_boundary(); std::cout << "Event budget checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
