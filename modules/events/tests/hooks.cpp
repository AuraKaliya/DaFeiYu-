#include <dafeiyu/hooks.hpp>
#include <dafeiyu/processing_queue.hpp>
#include <dafeiyu/owner_scheduler.hpp>
#include <iostream>
namespace {
using namespace dfy::runtime;
using namespace dfy::events;
using namespace std::chrono_literals;
// 在观察隔离、控制许可或退出边界违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 创建没有正文的事实事件，测试只关注控制及观察责任。
std::shared_ptr<const StructValue> payload() {
    const auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.hook",1,{}, {0,32}));
    return std::make_shared<const StructValue>(StructValue::create(type,{},0));
}
// 验证观察接纳超限不传播到已完成业务，也不产生部分交付。
void observations() {
    auto store = std::make_shared<EventStore>(4,0); BroadcastChannel channel(store,1,1); auto receiver = channel.subscribe();
    OperationSource business; business.settle(OperationState::succeeded);
    check(observe(channel,payload()).status == ObservationStatus::accepted,"observation acceptance failed");
    const auto rejected = observe(channel,payload());
    check(rejected.status == ObservationStatus::overloaded && !rejected.event.sequence && receiver->size() == 1 &&
        business.operation()->state() == OperationState::succeeded,"observer overload changed business result or delivered partly");
    check(observe(channel,{}).status == ObservationStatus::invalid,"invalid observation did not report separately");
}
// 验证控制参与者经调度运行，逻辑允许和否决均不冒充实际退出。
// 步骤：延后执行控制处理 → 全部允许后观察独立退出 → 否决并传播取消 → 验证截止先于迟到结果。
void controls() {
    auto store = std::make_shared<EventStore>(16,0); BroadcastChannel channel(store,1,1); auto receiver = channel.subscribe();
    channel.publish(payload()); auto data = receiver->poll().event; const ControlGate::TimePoint start{};
    ProcessingQueue queue(2,false); auto a = queue.submit(data), b = queue.submit(data); ControlGate gate({a,b},start+10s);
    OwnerScheduler scheduler(1); const auto owner = scheduler.add_owner(1,2);
    for (const auto& item : {a,b}) scheduler.post(owner,[&,item] { queue.start(item); queue.complete(item,OperationState::succeeded); });
    gate.advance(start); check(gate.decision() == ControlDecision::pending,"control allowed without responses");
    scheduler.run_one(); gate.advance(start+1s); check(gate.decision() == ControlDecision::pending,"control allowed with missing participant");
    scheduler.run_one(); gate.advance(start+2s);
    check(gate.decision() == ControlDecision::allowed && !gate.exited(),"control decision merged with resource exit");
    queue.finish(a); queue.finish(b); gate.advance(start+20s);
    check(gate.decision() == ControlDecision::allowed && gate.exited(),"later deadline overwrote committed permit");
    bool duplicate = false, backwards = false;
    try { ControlGate invalid({a,a},start+1s); } catch (const std::invalid_argument&) { duplicate = true; }
    try { gate.advance(start); } catch (const std::invalid_argument&) { backwards = true; }
    check(duplicate && backwards && gate.decision() == ControlDecision::allowed,"duplicate participation or backward clock changed decision");

    ProcessingQueue rejected(2,false); auto deny = rejected.submit(data), pending = rejected.submit(data); ControlGate denial({deny,pending},start+10s);
    rejected.start(deny); rejected.complete(deny,OperationState::failed); denial.advance(start);
    check(denial.decision() == ControlDecision::denied && pending->exit()->cancellation_reason() == CancelReason::owner_stop && !denial.exited(),
        "control failure allowed business or forged cleanup");
    rejected.finish(deny); rejected.advance(); check(denial.exited(),"control cancellation did not withdraw pending work");

    ProcessingQueue slow(1); auto work = slow.submit(data); slow.advance(); slow.start(work); ControlGate timeout({work},start+1s);
    timeout.advance(start+1s); check(timeout.decision() == ControlDecision::timed_out && !timeout.exited(),"deadline did not separate decision from running exit");
    slow.complete(work,OperationState::succeeded); slow.finish(work); timeout.advance(start+2s);
    check(timeout.decision() == ControlDecision::timed_out && timeout.exited(),"late permit overwrote deadline");
    ProcessingQueue cancelled(1); const auto waiting = cancelled.submit(data); ControlGate stop({waiting},start+1s);
    check(stop.cancel(CancelReason::user) == CancelDisposition::recorded && stop.decision() == ControlDecision::cancelled && !stop.exited(),
        "explicit gate cancellation forged processing exit");
    cancelled.advance(); stop.advance(start);
    check(stop.exited() && waiting->exit()->cancellation_reason() == CancelReason::user,"gate cancellation lost first reason or unstarted withdrawal");
}
}
// 验收普通观察失败隔离以及显式控制门的期限、取消和实际退出。
// 步骤：验证观察回执 → 验证受调度控制决策 → 报告结果。
int main() {
    try { observations(); controls(); std::cout << "Hook checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
