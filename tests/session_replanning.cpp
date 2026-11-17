#include "support/scripted_model.hpp"
#include <dafeiyu/session.hpp>
#include <atomic>
#include <iostream>
#include <thread>

namespace {
using namespace dfy::agent;
using namespace dfy::runtime;
using namespace dfy::model;
using namespace dfy::tests;
// 报告正式重规划与版本、执行边界不一致。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 验证旧版本、错绑或越过步骤边界的重规划请求被拒绝。
void rejects(const auto& action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid replan request accepted");
}
// 创建固定两步候选，可显式修改尾部或构造非法前缀。
Response proposal(bool revised = false, bool broken_prefix = false) {
    return {{Role::assistant,std::string(R"({"schema_version":1,"steps":[{"id":"first","objective":")")+
        (broken_prefix ? "changed" : "first")+R"(","completion_criterion":"checked","tools":[]},{"id":"last","objective":")"+
        (revised ? "revised" : "last")+R"(","completion_criterion":"checked","tools":[]}]})"}};
}
// 有界推进正式会话，固定响应必须达到明确的宿主观察状态。
void until(Session& session, const auto& ready) {
    const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (!ready() && std::chrono::steady_clock::now() < end) { session.advance(); std::this_thread::yield(); }
    check(ready(),"replan did not reach expected state");
}
// 验证重规划仅替换尾部，原验证证据、预算及模型退出仍属于同一运行。
// 步骤：完成并验证首步 → 请求一次尾部修订 → 校验前缀或停止 → 使用新版本执行剩余步骤。
void scenarios(unsigned mode) {
    ScriptStep generation; generation.response = proposal(); ScriptStep first; first.response = Response{{Role::assistant,"first output"}};
    ScriptStep revision; revision.response = proposal(true,mode == 1); ScriptStep last; last.response = Response{{Role::assistant,"last output"}};
    ScenarioSpec spec{1,"session_replan",1,{},{generation,first},2};
    if (mode < 2) { spec.steps.push_back(revision); spec.expected_model_requests = 3; }
    if (mode == 0) { spec.steps.push_back(last); spec.expected_model_requests = 4; }
    ScriptedModel model(std::move(spec)); const auto fixed = model.backend(); std::atomic<bool> protected_prefix = false;
    Backend backend{[&](const Config& config,const Request& input) {
        if (input.messages.back().content.find("\"completed_prefix\":1") != std::string::npos)
            protected_prefix = input.tools.empty() && input.messages.back().content.find("\"revision_guidance\":\"adjust tail\"") != std::string::npos;
        return fixed.complete(config,input);
    }};
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    Session session({},channel,backend); ConfirmationCaller caller, stranger; RunOptions options;
    options.now = model.clock(); options.planning = PlanLimits{16,2,16*1024}; options.confirmation_caller = caller;
    const auto request = session.begin("host goal",options);
    until(session,[&] { return request->plan()->definition && request->plan()->steps[0].waiting == PlanWait::verification; });
    const auto original = request->plan()->definition->binding(); const auto first_key = request->plan()->steps[0].key;
    rejects([&] { session.replan(caller,original,"adjust tail"); }); session.confirm_plan_step(caller,first_key,"first checked");
    rejects([&] { session.replan(stranger,original,"adjust tail"); }); auto stale = original; ++stale.goal_revision;
    rejects([&] { session.replan(caller,stale,"adjust tail"); });
    const auto target = session.replan(caller,original,"adjust tail");
    check(target.plan_revision == 2 && target.run == request->run_id() && request->diagnostics().model_requests == 2 &&
        session.replan(caller,original,"adjust tail") == target,"replan inline execution or duplicate request");
    rejects([&] { session.replan(caller,original,"changed command"); });
    if (mode >= 2) {
        if (mode == 2) request->request_cancel(CancelReason::user); else model.advance_clock(options.timeout);
    }
    if (mode != 0) {
        until(session,[&] { return !session.busy(); });
        check(request->operation()->state() == (mode == 1 ? OperationState::failed : OperationState::cancelled) &&
            request->plan()->definition->binding() == original && request->plan()->steps[0].state == PlanStepState::completed,
            "invalid or stopped replan overwrote original facts");
    } else {
        until(session,[&] { return request->plan()->definition->binding() == target; });
        check(protected_prefix && request->plan()->steps[0].key == first_key && request->plan()->steps[0].evidence == "first checked",
            "replan lost protected prefix or host evidence");
        rejects([&] { session.replan(caller,target,"over revision budget"); });
        until(session,[&] { return request->plan()->steps[1].waiting == PlanWait::verification; });
        auto old_key = request->plan()->steps[1].key; old_key.plan = original;
        rejects([&] { session.confirm_plan_step(caller,old_key,"stale"); });
        session.confirm_plan_step(caller,request->plan()->steps[1].key,"last checked"); until(session,[&] { return !session.busy(); });
        check(request->answer() == "last output" && request->plan_output(first_key)->text == "first output","replan changed original output");
    }
    model.verify_complete(); const auto attempted = model.report().attempted;
    check(session.replan(caller,original,"adjust tail") == target && model.report().attempted == attempted,"terminal replan receipt restarted work");
}
// 验证规划模型尚未退出时的宿主推进不会重复提交或覆盖原请求。
// 步骤：阻塞固定模型响应 → 多次推进原运行 → 放行同一请求 → 查询候选并取消收尾。
void pending_model() {
    ScriptStep generation; generation.response = proposal(); ScriptedModel model({1,"pending_planning",1,{},{generation},1});
    const auto fixed = model.backend(); std::atomic<bool> entered = false, release = false;
    Backend backend{[&](const Config& config,const Request& input) {
        entered = true; const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while (!release && std::chrono::steady_clock::now() < end) std::this_thread::yield();
        return fixed.complete(config,input);
    }};
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    Session session({},channel,backend); RunOptions options; options.planning = PlanLimits{}; options.now = model.clock();
    const auto request = session.begin("host goal",options); until(session,[&] { return entered.load(); });
    for (unsigned repeat = 0; repeat < 16; ++repeat) session.advance();
    const bool pending = request->operation()->state() == OperationState::pending && request->diagnostics().model_requests == 1;
    release = true; check(pending,"pending planning submitted another model or lost original run");
    until(session,[&] { return request->plan()->definition != nullptr; }); request->request_cancel(CancelReason::user);
    until(session,[&] { return !session.busy(); }); model.verify_complete();
}
}
// 使用固定候选和原模型线程验证有限重规划，不测试真实模型规划质量。
int main() {
    try { for (unsigned mode = 0; mode < 4; ++mode) scenarios(mode); pending_model(); std::cout << "Session replanning checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
