#include "support/scripted_model.hpp"
#include <dafeiyu/observation_controller.hpp>
#include <iostream>
#include <thread>

namespace {
using namespace dfy::agent;
using namespace dfy::runtime;
using namespace dfy::model;
using namespace dfy::tests;
// 报告自动接纳、优先级或停止边界不符。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 有界推进原会话，控制器不另建执行循环。
void drain(Session& session) {
    const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (session.busy() && std::chrono::steady_clock::now() < end) { session.advance(); std::this_thread::yield(); }
    check(!session.busy(),"observed session did not exit");
}
// 创建合成空状态，载荷不持有设备或插件代码。
std::shared_ptr<const StructValue> payload() {
    auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("fixture.empty",1,{}, {1,64}));
    return std::make_shared<const StructValue>(StructValue::create(type,{},64));
}
// 验证用户输入优先、来源绑定、去重、频率与显式停止。
// 步骤：先完成用户运行 → 接纳两次不同采样 → 拒绝重复及过快触发 → 停止自己的运行并允许后续用户问答。
void dispatch() {
    ScriptStep user; user.last_text = "user"; user.response = Response{{Role::assistant,"answer"}};
    ScriptStep observed; observed.response = Response{{Role::assistant,"observed"}};
    ScriptedModel model({1,"controller_dispatch",1,{},{user,observed,observed,user},4});
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(16,64),1,8);
    Session session({},channel,model.backend()); ObservationSource source(1,ObservationTrust::external_data); ObservationInbox inbox({source.id()});
    const auto sample = [&](std::uint64_t revision) { const auto now = model.clock()(); inbox.accept(source.capture({"object",1},revision,now,now+std::chrono::seconds(1),payload()),now); };
    sample(1); RunOptions options; options.now = model.clock(); options.policy.allow_observation = true;
    ObservationController controller(session,inbox,{source.id(),{"object",1},1,7,2,std::chrono::milliseconds(10),ObservationTime{}+std::chrono::seconds(1)},"host goal",options);
    check(controller.poll().decision == TriggerDecision::disabled,"controller auto-enabled"); controller.enable();
    check(controller.poll(true).decision == TriggerDecision::user_pending && !controller.runs(),"user input lost priority");
    auto manual = session.begin("user"); check(controller.poll().decision == TriggerDecision::busy && !controller.runs(),"busy user run replaced"); drain(session);
    auto first = controller.poll(); check(first.request && first.request->diagnostics().source == RunOrigin::observation_rule &&
        first.request->diagnostics().goal_revision == 7,"trigger lost host origin or goal"); drain(session);
    check(controller.poll().decision == TriggerDecision::duplicate && controller.runs() == 1,"same sample replayed");
    sample(2); check(controller.poll().decision == TriggerDecision::throttled,"trigger ignored frequency"); model.advance_clock(std::chrono::milliseconds(10));
    auto second = controller.poll(); check(second.request && controller.runs() == 2,"second sample not admitted");
    controller.stop(); drain(session); check(second.request->operation()->state() == OperationState::cancelled,"stop did not cancel owned run");
    auto after = session.begin("user"); drain(session); check(after->answer() == "answer" && !session.closed(),"controller stopped user session");
    model.verify_complete();
}
// 验证停止回调和禁止的变更范围都在模型调用前拒绝，不消费观察预算。
// 步骤：在首个控制点停止规则 → 核对零接纳 → 禁用变更工具仍拒绝原授予范围。
void refusals() {
    ScriptedModel model({1,"controller_refused",1,{}, {},0});
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    Session session({},channel,model.backend()); ObservationSource source(1,ObservationTrust::host_verified); ObservationInbox inbox({source.id()});
    inbox.accept(source.capture({"object",1},1,{},ObservationTime{}+std::chrono::seconds(1),payload()),{});
    const ObservationRule rule{source.id(),{"object",1},1,1,1,std::chrono::milliseconds(1),ObservationTime{}+std::chrono::seconds(1)};
    RunOptions options; options.now = model.clock(); options.policy.allow_observation = true; ObservationController* active = nullptr;
    options.extension = [&](auto,const auto&) { active->stop(); return PolicyDecision{PolicyVerdict::allow,PolicyReason::none}; };
    ObservationController controller(session,inbox,rule,"goal",options); active = &controller; controller.enable();
    bool rejected = false; try { controller.poll(); } catch (const std::exception&) { rejected = true; }
    check(rejected && !controller.runs() && !session.busy(),"callback stop admitted model or leaked reservation");
    ToolCatalog catalog; const auto registration = catalog.add({"fixture/change",1,1,{"change","fixture",R"({"type":"object","properties":{},"additionalProperties":false})"},true,false},
        [](const auto&) { return completed_tool({true,ToolEffect::applied,"",ToolError::none}); });
    const std::vector<std::string> ids{"fixture/change"}; options.tools = catalog.snapshot(ids,true); catalog.enable(registration,false);
    rejected = false; try { ObservationController forbidden(session,inbox,rule,"goal",options); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected,"disabled change tool hid an unsafe fixed scope"); model.verify_complete();
}
// 验证实际接纳之后时钟失败仍记账并可查询，原会话继续完成真实退出。
// 步骤：让接纳后的时钟失败 → 核对原运行仍可查询且计数 → 推进真实退出并保持停止。
void admission_clock_failure() {
    ScriptStep step; step.response = Response{{Role::assistant,"late"}}; ScriptedModel model({1,"controller_clock",1,{},{step},1});
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    Session session({},channel,model.backend()); ObservationSource source(1,ObservationTrust::external_data); ObservationInbox inbox({source.id()});
    inbox.accept(source.capture({"object",1},1,{},ObservationTime{}+std::chrono::seconds(1),payload()),{});
    RunOptions options; options.policy.allow_observation = true;
    options.now = [&] { if (session.busy()) throw std::runtime_error("clock failed after admission"); return ObservationTime{}; };
    ObservationController controller(session,inbox,{source.id(),{"object",1},1,1,1,std::chrono::milliseconds(1),ObservationTime{}+std::chrono::seconds(1)},"goal",options);
    controller.enable(); bool rejected = false; try { controller.poll(); } catch (const std::runtime_error&) { rejected = true; }
    check(rejected && controller.runs() == 1 && controller.current() && controller.current()->exit()->state() == OperationState::pending,
        "post-admission clock failure lost accepted run or budget");
    const auto current = controller.current(); drain(session); model.verify_complete();
    check(current->operation()->state() == OperationState::cancelled && current->exit()->state() == OperationState::succeeded &&
        controller.poll().decision == TriggerDecision::stopped,"clock failure prevented cleanup or restarted trigger");
}
}
// 使用正式 Session 和既有模型替身，不采集设备或运行真实模型。
int main() {
    try { dispatch(); refusals(); admission_clock_failure(); std::cout << "Observation controller checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
