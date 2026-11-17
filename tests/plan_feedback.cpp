#include "support/scripted_model.hpp"
#include <dafeiyu/session.hpp>
#include <iostream>
#include <thread>

namespace {
using namespace dfy::agent;
using namespace dfy::runtime;
using namespace dfy::model;
using namespace dfy::tests;
// 报告反馈把旧采样、错误身份或未满足条件当作完成。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 验证在途、旧目标或错误调用者无法签发或消费反馈。
void rejects(const auto& action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid plan feedback accepted");
}
// 创建明确的临时状态，不由模型文字提供完成证据。
std::shared_ptr<const StructValue> payload(std::uint64_t state) {
    const FieldDescriptor fields[] = {{"state",ValueKind::uint64,0}};
    auto schema = std::make_shared<const StructDescriptor>(StructDescriptor::create("fixture.feedback",1,fields,{1,64}));
    const NamedValue values[] = {{"state",Value::from_uint64(state)}};
    return std::make_shared<const StructValue>(StructValue::create(schema,values,64));
}
// 有界推进到可验证状态，读取本身不推进模型工作。
void until(Session& session, const auto& ready) {
    const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (!ready() && std::chrono::steady_clock::now() < end) { session.advance(); std::this_thread::yield(); }
    check(ready(),"plan feedback scenario stalled");
}
// 验证实际退出后的新采样才能完成原步骤，未知和未满足保持等待且不重放动作。
// 步骤：拒绝提前签发 → 固定原反馈条件 → 检查旧值和未满足 → 接收新证据或注入停止 → 核对原运行。
void scenario(unsigned mode) {
    ObservationSource source(1,ObservationTrust::host_verified); ObservationInbox inbox({source.id()});
    ScriptStep generation; generation.response = Response{{Role::assistant,R"({"schema_version":1,"steps":[{"id":"one","objective":"target","completion_criterion":"host checks","tools":[]}]})"}};
    ScriptStep answer; answer.response = Response{{Role::assistant,"model claims done"}};
    ScriptedModel model({1,"plan_feedback",1,{},{generation,answer},2});
    const auto sample = [&](std::uint64_t revision, std::uint64_t state) {
        const auto now = model.clock()(); return source.capture({"object",1},revision,now,now+std::chrono::seconds(1),payload(state));
    };
    inbox.accept(sample(1,0),{}); const auto baseline = inbox.guard(inbox.latest(source.id(),"object"),{});
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    Session session({},channel,model.backend()); ConfirmationCaller caller, stranger; RunOptions options;
    options.planning = PlanLimits{}; options.now = model.clock(); options.confirmation_caller = caller;
    const auto request = session.begin("host goal",options); until(session,[&] { return request->plan()->definition != nullptr; });
    const auto key = request->plan()->steps[0].key;
    rejects([&] { session.request_plan_feedback(caller,key,baseline,"state",Value::from_uint64(1)); });
    until(session,[&] { return request->plan()->steps[0].waiting == PlanWait::verification; });
    rejects([&] { session.request_plan_feedback(stranger,key,baseline,"state",Value::from_uint64(1)); });
    const auto ticket = session.request_plan_feedback(caller,key,baseline,"state",Value::from_uint64(1)); auto copied = ticket;
    const auto transferred = std::move(copied); rejects([&] { copied.assess(baseline,{}); });
    if (!mode) {
        for (unsigned issued = 1; issued < 64; ++issued) session.request_plan_feedback(caller,key,baseline,"state",Value::from_uint64(1));
        rejects([&] { session.request_plan_feedback(caller,key,baseline,"state",Value::from_uint64(1)); });
    }
    check(transferred.id() == ticket.id() && session.verify_plan_feedback(caller,ticket,baseline).issue == ObservationEvidenceIssue::not_resampled,
        "old observation completed post-work feedback");
    inbox.accept(sample(2,0),model.clock()()); const auto unmet = inbox.guard(inbox.latest(source.id(),"object"),model.clock()());
    check(session.verify_plan_feedback(caller,ticket,unmet).match == ObservationMatch::not_satisfied && request->operation()->state() == OperationState::pending,
        "unmet condition completed run");
    model.advance_clock(std::chrono::milliseconds(1)); inbox.accept(sample(3,1),model.clock()()); const auto current = inbox.guard(inbox.latest(source.id(),"object"),model.clock()());
    rejects([&] { session.verify_plan_feedback(stranger,ticket,current); });
    if (mode == 1) session.revise_goal(caller,key.plan,"new goal");
    if (mode == 2) request->request_cancel(CancelReason::user);
    if (mode == 3) model.advance_clock(options.timeout);
    if (mode) {
        rejects([&] { session.verify_plan_feedback(caller,ticket,current); }); request->request_cancel(CancelReason::user);
    } else {
        const auto result = session.verify_plan_feedback(caller,transferred,current);
        check(result.match == ObservationMatch::satisfied && result.revision == 3 && request->operation()->state() == OperationState::pending &&
            request->plan()->steps[0].evidence.starts_with("feedback/"),"feedback lost evidence or advanced run inline");
    }
    until(session,[&] { return !session.busy(); }); model.verify_complete();
    check(request->operation()->state() == (mode ? OperationState::cancelled : OperationState::succeeded),"feedback changed wrong run outcome");
}
}
// 使用固定规划和合成状态验证正式反馈入口，不依赖真实模型判断。
int main() {
    try { for (unsigned mode = 0; mode < 4; ++mode) scenario(mode); std::cout << "Plan feedback checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
