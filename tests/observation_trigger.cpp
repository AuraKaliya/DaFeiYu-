#include <dafeiyu/observation_trigger.hpp>
#include <iostream>

namespace {
using namespace dfy::agent;
using namespace dfy::runtime;
// 报告规则开启、预算或观察去重契约错误。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 核对冲突的接纳和停止后恢复必须拒绝。
void rejects(const auto& action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid trigger operation accepted");
}
// 准备空的宿主结构值，不引入模型或设备采样。
std::shared_ptr<const StructValue> payload() {
    auto descriptor = std::make_shared<const StructDescriptor>(StructDescriptor::create("fixture.empty",1,{}, {1,64}));
    return std::make_shared<const StructValue>(StructValue::create(descriptor,{},64));
}
// 验证显式开启、撤回、实际接纳计数、重复修订及有限频率。
// 步骤：开启并撤回预留 → 记录实际接纳 → 拒绝重复与过快采样 → 停止后保留原记账。
void admission() {
    const ObservationTime now{}; ObservationSource source(1,ObservationTrust::external_data); ObservationInbox inbox({source.id()});
    const auto sample = [&](std::uint64_t revision) {
        inbox.accept(source.capture({"object",1},revision,now,now+std::chrono::seconds(10),payload()),now);
        return inbox.latest(source.id(),"object");
    };
    auto first = sample(1); ObservationRule rule{source.id(),{"object",1},1,1,2,std::chrono::milliseconds(100),now+std::chrono::seconds(10)};
    ObservationTrigger trigger(rule,now); check(trigger.inspect(*first,now) == TriggerDecision::disabled,"trigger enabled by default");
    trigger.enable(); const auto cancelled = *trigger.reserve(*first,now).ticket;
    check(trigger.inspect(*first,now) == TriggerDecision::busy,"parallel reservation allowed"); trigger.abort(cancelled);
    const auto ticket = *trigger.reserve(*first,now).ticket; const auto receipt = trigger.commit(ticket,{1,1},now);
    check(trigger.commit(ticket,{1,1},now).run == receipt.run && trigger.runs() == 1,"duplicate receipt consumed budget");
    rejects([&] { trigger.commit(ticket,{1,2},now); });
    check(trigger.inspect(*first,now) == TriggerDecision::duplicate,"same observation triggered twice");
    auto second = sample(2); check(trigger.inspect(*second,now) == TriggerDecision::throttled,"frequency limit ignored");
    const auto next = *trigger.reserve(*second,now+std::chrono::milliseconds(100)).ticket;
    rejects([&] { trigger.commit(next,{1,1},now+std::chrono::milliseconds(100)); });
    trigger.stop(); trigger.commit(next,{1,2},now+std::chrono::milliseconds(100));
    check(trigger.runs() == 2 && trigger.inspect(*second,now) == TriggerDecision::stopped && !trigger.guard()(now),"stop lost real admission or remained open");
    rejects([&] { trigger.enable(); });
}
// 验证断档、到期、对象代次、运行次数及销毁后的控制门。
// 步骤：拒绝断档及到期 → 建立完整基线并耗尽预算 → 检查销毁和代次边界。
void boundaries() {
    const ObservationTime now{}; ObservationSource source(1,ObservationTrust::host_verified); ObservationInbox inbox({source.id()});
    inbox.accept(source.capture({"object",1},3,now,now+std::chrono::seconds(2),payload()),now);
    const ObservationRule rule{source.id(),{"object",1},1,1,1,std::chrono::milliseconds(1),now+std::chrono::seconds(1)};
    std::function<bool(ObservationTime)> gate;
    {
        ObservationTrigger trigger(rule,now); trigger.enable(); gate = trigger.guard(); const auto value = inbox.latest(source.id(),"object");
        check(trigger.inspect(*value,now) == TriggerDecision::gap && trigger.inspect(*value,rule.deadline) == TriggerDecision::expired,"gap or deadline ignored");
        const auto refresh = *inbox.refresh_ticket(source.id(),"object"); inbox.refresh(refresh,source.capture({"object",1},4,now,now+std::chrono::seconds(2),payload()),now);
        const auto ready = inbox.latest(source.id(),"object"); const auto ticket = *trigger.reserve(*ready,now).ticket; trigger.commit(ticket,{1,1},now);
        check(trigger.inspect(*ready,now) == TriggerDecision::run_limit,"run budget ignored");
    }
    check(!gate(now),"destroyed trigger left a live gate");
    ObservationTrigger mismatch(rule,now); mismatch.enable(); inbox.accept(source.capture({"object",2},1,now,now+std::chrono::seconds(2),payload()),now);
    check(mismatch.inspect(*inbox.latest(source.id(),"object"),now) == TriggerDecision::wrong_object,"new object generation silently gained scope");
    auto invalid = rule; invalid.maximum_runs = 0; rejects([&] { ObservationTrigger bad(invalid,now); });
}
}
// 只验证合成观察与纯触发接纳，不提交任何模型或工具。
int main() {
    try { admission(); boundaries(); std::cout << "Observation trigger checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
