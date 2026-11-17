#include "support/scripted_model.hpp"
#include <dafeiyu/session.hpp>
#include <iostream>
#include <thread>

namespace {
using namespace dfy::agent;
using namespace dfy::runtime;
using namespace dfy::model;
using namespace dfy::tests;
// 报告运行内观察替换或原预算边界错误。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 验证旧步骤、错误主体或不完整采样不能更新原运行。
void rejects(const auto& action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid observation rebind accepted");
}
// 创建无设备的确定状态载荷。
std::shared_ptr<const StructValue> payload() {
    auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("fixture.rebind",1,{}, {1,64}));
    return std::make_shared<const StructValue>(StructValue::create(type,{},64));
}
// 有界推进原 Session，刷新命令不替代实际工作推进。
void until(Session& session, const auto& ready) {
    const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (!ready() && std::chrono::steady_clock::now() < end) { session.advance(); std::this_thread::yield(); }
    check(ready(),"observation rebind scenario stalled");
}
// 验证步骤边界的完整替换、幂等回执、错绑和停止，命令不直接推进下一步骤。
// 步骤：进入待验证边界 → 请求新采样 → 拒绝旧值和错主体 → 接纳或停止 → 核对原状态和调用数。
void scenario(unsigned mode) {
    ScriptStep generation; generation.response = Response{{Role::assistant,R"({"schema_version":1,"steps":[{"id":"one","objective":"read","completion_criterion":"host verifies","tools":[]}]})"}};
    ScriptStep answer; answer.response = Response{{Role::assistant,"done"}}; ScriptedModel model({1,"observation_rebind",1,{},{generation,answer},2});
    ObservationSource source(1,ObservationTrust::host_verified); ObservationInbox inbox({source.id()});
    const auto sample = [&](std::uint64_t revision) { const auto now = model.clock()(); inbox.accept(source.capture({"object",1},revision,now,now+std::chrono::seconds(1),payload()),now); };
    const auto observations = [&] { return ActionObservations({inbox.guard(inbox.latest(source.id(),"object"),model.clock()())}); };
    sample(1); const auto original = observations(); ConfirmationCaller caller, stranger;
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(16,64),1,8);
    Session session({},channel,model.backend()); RunOptions options; options.planning = PlanLimits{}; options.now = model.clock();
    options.confirmation_caller = caller; options.action_observations = original; options.observation_rebind_limit = 2;
    const auto request = session.begin("goal",options); until(session,[&] { return request->plan()->definition != nullptr; }); const auto key = request->plan()->steps[0].key;
    rejects([&] { session.request_observation_rebind(caller,key); });
    until(session,[&] { return request->plan()->steps[0].waiting == PlanWait::verification; });
    rejects([&] { session.request_observation_rebind(stranger,key); });
    const auto ticket = session.request_observation_rebind(caller,key); auto duplicate = ticket; const auto moved = std::move(duplicate);
    rejects([&] { session.rebind_observations(caller,duplicate,original); });
    rejects([&] { session.rebind_observations(caller,ticket,observations()); });
    session.request_observation_rebind(caller,key); rejects([&] { session.request_observation_rebind(caller,key); });
    sample(2); const auto next = observations(); rejects([&] { session.rebind_observations(stranger,ticket,next); });
    if (mode == 1) request->request_cancel(CancelReason::user);
    if (mode == 2) model.advance_clock(options.timeout);
    if (mode == 3) session.revise_goal(caller,key.plan,"new goal");
    if (mode) {
        rejects([&] { session.rebind_observations(caller,ticket,next); }); request->request_cancel(CancelReason::user);
    } else {
        const auto receipt = session.rebind_observations(caller,moved,next);
        check(receipt.binding == next.binding() && receipt.previous_binding == original.binding() && receipt.observations[0].revision == 2 &&
            request->plan()->steps[0].waiting == PlanWait::verification && model.report().attempted == 2,"replacement advanced or lost original identity");
        check(session.rebind_observations(caller,ticket,next).binding == receipt.binding && request->observation_rebinds().size() == 1,"retry republished observations");
        rejects([&] { session.rebind_observations(caller,ticket,observations()); });
        session.confirm_plan_step(caller,key,"checked");
    }
    until(session,[&] { return !session.busy(); }); model.verify_complete();
    if (!mode) check(session.rebind_observations(caller,ticket,next).binding == next.binding(),"finished receipt cannot be retrieved");
    check(request->diagnostics().model_requests == 2 && request->operation()->state() == (mode ? OperationState::cancelled : OperationState::succeeded),"replacement reset original run");
}
}
// 仅以预设模型响应和合成状态验证原运行替换，不调用真实模型。
int main() {
    try { for (unsigned mode = 0; mode < 4; ++mode) scenario(mode); std::cout << "Session observation rebind checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
