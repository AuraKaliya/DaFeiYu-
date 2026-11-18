#include "support/compiled_replay.hpp"
#include <dafeiyu/session.hpp>
#include <dafeiyu/run_trace.hpp>
#include <atomic>
#include <iostream>
#include <thread>

namespace {
using namespace dfy::agent;
using namespace dfy::runtime;
using namespace dfy::model;
using namespace dfy::tests;
struct GoalCase { std::string_view id; bool confirmed, limited; };
constexpr GoalCase cases[]{
    {"goal_revision",false,false},{"goal_revision_confirmed",true,false},{"goal_revision_limit",false,true}};
// 报告目标更新、历史状态或原运行预算错误。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 验证未授权、非边界或过量目标修订被拒绝。
void rejects(const auto& action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid goal revision accepted");
}
// 创建固定单步计划，目标版本只能由原宿主请求绑定。
Response proposal() {
    return {{Role::assistant,R"({"schema_version":1,"steps":[{"id":"step","objective":"work","completion_criterion":"host checks","tools":[]}]})"}};
}
// 有界推进直到明确状态，不依赖模型或诊断查询主动调度。
void until(Session& session, const auto& ready) {
    const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (!ready() && std::chrono::steady_clock::now() < end) { session.advance(); std::this_thread::yield(); }
    check(ready(),"goal revision did not reach expected state");
}
// 验证步骤边界目标更新保留原未验证或已验证工作，新目标不重置原总模型预算。
// 步骤：取得原步骤结果 → 明确替换目标 → 拒绝旧版本及超量更新 → 核对新调用与旧事实。
void revised(const GoalCase& example, std::ostream* evidence = nullptr) {
    const auto [id,confirmed,limited] = example;
    ScriptStep generation; generation.response = proposal(); ScriptStep first; first.response = Response{{Role::assistant,"old output"}};
    ScriptStep last; last.response = Response{{Role::assistant,"new output"}};
    ScenarioSpec spec{1,std::string(id),1,{},{generation,first},2};
    if (!limited) { spec.steps.push_back(generation); spec.steps.push_back(last); spec.expected_model_requests = 4; }
    ScriptedModel model(std::move(spec)); const auto fixed = model.backend(); std::atomic<bool> saw_goal = false, saw_facts = false;
    Backend backend{[&](const Config& config,const Request& input) {
        if (input.messages.front().role == Role::system && input.messages.back().content.find("\"goal\":\"new goal\"") != std::string::npos) saw_goal = true;
        if (input.messages.front().role == Role::user && input.messages.size() > 4)
            saw_facts = input.messages[2].content == "old output" && input.messages[3].content.ends_with("new goal");
        return fixed.complete(config,input);
    }};
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    Session session(fixed_replay_model(),channel,backend); ConfirmationCaller caller, stranger; RunOptions options;
    options.version_labels = compiled_replay_labels("agent.plan-request-v1");
    options.planning = PlanLimits{}; options.now = model.clock(); options.confirmation_caller = caller; options.goal_update_limit = 1;
    if (limited) options.policy.model_limit = 2;
    const auto request = session.begin("old goal",options);
    until(session,[&] { return request->plan()->definition && request->plan()->steps[0].waiting == PlanWait::verification; });
    const auto original = request->plan()->definition->binding(); const auto old_key = request->plan()->steps[0].key;
    if (confirmed) session.confirm_plan_step(caller,old_key,"old checked");
    rejects([&] { session.revise_goal(stranger,original,"new goal"); });
    rejects([&] { session.revise_goal(caller,original," "); });
    check(request->plan_history().empty() && request->diagnostics().goal_revision == 1,"bad revision changed original plan");
    const auto target = session.revise_goal(caller,original,"new goal");
    check(target == PlanBinding{request->run_id(),2,0} && request->diagnostics().model_requests == 2 &&
        session.revise_goal(caller,original,"new goal") == target,"goal update executed model inline or repeated revision");
    rejects([&] { session.revise_goal(caller,original,"different goal"); });
    rejects([&] { session.replan(caller,original,"old target"); });
    if (confirmed) session.confirm_plan_step(caller,old_key,"old checked");
    else rejects([&] { session.confirm_plan_step(caller,old_key,"late old verification"); });
    const auto archive = request->plan_history();
    check(archive.size() == 1 && archive[0].stopped && archive[0].definition->binding() == original &&
        archive[0].steps[0].state == (confirmed ? PlanStepState::completed : PlanStepState::running) && request->plan_output(old_key)->text == "old output",
        "goal update erased old work or falsely completed it");
    if (limited) {
        until(session,[&] { return !session.busy(); });
        check(request->operation()->state() == OperationState::failed && request->diagnostics().stop_reason == PolicyReason::model_limit,
            "new goal reset original model limit");
    } else {
        until(session,[&] { return request->plan()->definition && request->plan()->steps[0].waiting == PlanWait::verification; });
        const auto key = request->plan()->steps[0].key;
        check(key.plan.goal_revision == 2 && key.plan.plan_revision == 1 && request->plan_output(key)->text == "new output","new goal reused old output or version");
        rejects([&] { session.revise_goal(caller,key.plan,"too many goals"); });
        session.confirm_plan_step(caller,key,"new checked"); until(session,[&] { return !session.busy(); });
        check(request->answer() == "new output" && session.message_count() == 6 && saw_goal && saw_facts,"goal revision lost committed context");
    }
    model.verify_complete(); const auto trace = run_timeline(*request,"goal-revision");
    check(review_timeline(trace).run_exited && trace.goal_revision == 2 && trace.model_contexts[0].goal_revision == 1 &&
        trace.model_contexts[1].goal_revision == 1 && (limited || trace.model_contexts[2].goal_revision == 2),"new target relabeled old model work");
    check(session.revise_goal(caller,original,"new goal") == target && request->plan_history()[0].steps[0].state == archive[0].steps[0].state,
        "terminal goal retry mutated original archive");
    if (evidence) write_compiled_replay(*evidence,model.report(),*request,0);
}
// 验证在途模型不能被目标更新替换，原请求退出后才允许接纳新目标。
// 步骤：阻塞当前步骤模型 → 拒绝更新且保持原版本 → 放行并接纳边界更新 → 取消尚未提交的新规划。
void inflight_boundary() {
    ScriptStep generation; generation.response = proposal(); ScriptStep answer; answer.response = Response{{Role::assistant,"old output"}};
    ScriptedModel model({1,"goal_boundary",1,{},{generation,answer},2}); const auto fixed = model.backend();
    std::atomic<bool> entered = false, release = false;
    Backend backend{[&](const Config& config,const Request& input) {
        if (input.messages.front().role != Role::system) {
            entered = true; const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(2);
            while (!release && std::chrono::steady_clock::now() < end) std::this_thread::yield();
        }
        return fixed.complete(config,input);
    }};
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    Session session({},channel,backend); ConfirmationCaller caller; RunOptions options;
    options.planning = PlanLimits{}; options.now = model.clock(); options.confirmation_caller = caller;
    const auto request = session.begin("old goal",options); until(session,[&] { return entered.load(); });
    const auto expected = request->plan()->definition->binding();
    bool rejected = false; try { session.revise_goal(caller,expected,"new goal"); } catch (const std::exception&) { rejected = true; }
    release = true; check(rejected && request->diagnostics().goal_revision == 1 && request->plan_history().empty(),"inflight goal was replaced");
    until(session,[&] { return request->plan()->steps[0].waiting == PlanWait::verification; });
    session.revise_goal(caller,expected,"new goal"); request->request_cancel(CancelReason::user);
    until(session,[&] { return !session.busy(); }); model.verify_complete();
    check(request->plan_history()[0].steps[0].result == OperationState::succeeded && request->operation()->state() == OperationState::cancelled,
        "goal boundary lost actual old model result");
}
}
// 用固定规划和文本响应验证用户目标更新，并可采集指定案例。
// 步骤：匹配显式采集名称或运行原矩阵 → 核对旧目标和实际退出 → 返回结果。
int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--capture") {
            const auto found = std::find_if(std::begin(cases),std::end(cases),[&](const auto& value) { return value.id == argv[2]; });
            check(found != std::end(cases),"unknown goal revision replay case");
            revised(*found,&std::cout);
        } else {
            check(argc == 1,"usage: --capture CASE");
            for (const auto& example : cases) revised(example);
            inflight_boundary();
            std::cout << "Session goal revision checks passed\n";
        }
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
