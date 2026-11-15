#include <dafeiyu/async_session.hpp>
#include <iostream>
#include <thread>
namespace {
using namespace dfy::runtime;
using namespace dfy::events;
using namespace dfy::agent;
// 在会话历史、异步通知或取消退出边界违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 核对精确拒绝类别，保留无关失败供入口报告。
template<class Error, class Action>
void rejects(Action action) { try { action(); } catch (const Error&) { return; } throw std::runtime_error("async session accepted invalid action"); }
// 有界推进真实后台模型，完成回合不需要执行观察消费者。
void drain(AsyncSession& session, const std::shared_ptr<TurnRequest>& request) {
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (request->exit()->state() == OperationState::pending && std::chrono::steady_clock::now() < deadline) { session.advance(); std::this_thread::yield(); }
    check(request->exit()->state() == OperationState::succeeded,"session did not observe actual model exit");
}
// 验证成对提交、失败保留和类型化事件快照，读取通知不参与问答完成。
// 步骤：成功及失败问答 → 拒绝结果超量 → 检查下一轮上下文 → 清空并核对事实序列。
void history() {
    auto store = std::make_shared<EventStore>(24,64); auto channel = std::make_shared<BroadcastChannel>(store,1,16); auto events = channel->subscribe();
    std::vector<dfy::model::Message> received;
    AsyncSession session({},channel,2,[&](const auto&,const auto& messages) {
        received = messages;
        if (messages.back().content == "fail") throw std::runtime_error("model: controlled failure");
        return std::string("answer:")+messages.back().content;
    });
    auto a = session.begin("first"); rejects<std::logic_error>([&] { (void)session.begin("busy"); }); drain(session,a);
    check(a->answer() == "answer:first" && session.message_count() == 2 && events->size() == 2,"success did not commit independently of observer");
    const auto success = a->diagnostics();
    check(success.attempt == a->attempt_id() && success.attempt.step == a->step_id() &&
        success.attempt.step.run == RunId{session.id(),a->turn()} && success.attempt.step.sequence == 1 &&
        success.attempt.sequence == 1,"single completion lost run, step or attempt association");
    check(success.session == session.id() && success.turn == 1 && success.result == OperationState::succeeded &&
        success.exit == OperationState::succeeded && success.failure == TurnFailure::none && !success.cancel_requested &&
        success.settled && success.exited && success.model.finished && success.model.submitted <= success.accepted &&
        success.accepted <= *success.settled && *success.model.finished <= *success.settled && *success.settled <= *success.exited,
        "successful turn lost identity, timing or original operation states");
    auto b = session.begin("fail"); drain(session,b);
    check(b->operation()->state() == OperationState::failed && session.message_count() == 2,"model failure polluted history");
    const auto failure = b->diagnostics();
    check(b->run_id() != a->run_id() && failure.attempt.step.run.sequence == 2 &&
        a->diagnostics().attempt == success.attempt,"failed turn reused or changed a prior run identity");
    check(failure.failure == TurnFailure::model && failure.model.outcome == dfy::model::CallOutcome::threw &&
        failure.model.request != success.model.request && success.turn == 1 && success.result == OperationState::succeeded,
        "model failure or next turn corrupted retained diagnostics");
    rejects<std::length_error>([&] { (void)session.begin("held"); }); a.reset(); b.reset();
    auto c = session.begin("second"); drain(session,c);
    check(c->turn() == 3 && received.size() == 3 && received[0].content == "first" && received[2].content == "second" &&
        session.message_count() == 4,"failed acceptance or model failure changed context or turn");
    session.clear(); check(!session.message_count(),"clear retained committed context");
    const std::vector<std::string> kinds{"turn_started","turn_completed","turn_started","turn_failed","turn_started","turn_completed","session_cleared"};
    const std::vector<std::uint64_t> counts{0,2,2,2,2,4,0};
    for (std::size_t i = 0; i < kinds.size(); ++i) {
        auto item = events->poll(); check(item.state == PollState::event,"session event missing"); const auto& value = *item.event.get().payload;
        check(value.find("kind")->as_string() == kinds[i] && value.find("session")->as_uint64() == session.id() &&
            value.find("messages")->as_uint64() == counts[i],"session event observed wrong commit boundary");
    }
    check(events->poll().state == PollState::empty,"duplicate terminal event emitted"); session.close(); check(session.closed(),"idle session did not close");
}
struct Release {
    std::promise<void>& promise; bool released = false;
    // 断言失败也解除后台替身，避免析构时等待无穷阻塞。
    ~Release() { if (!released) promise.set_value(); }
    // 一次性允许后台替身实际结束。
    void open() { promise.set_value(); released = true; }
};
// 验证观察背压不改变模型成功，取消先产生业务终态但仍等待真实退出。
// 步骤：制造观察背压 → 核对业务成功 → 阻塞后台并关闭会话 → 观察取消后实际退出。
void isolation() {
    auto store = std::make_shared<EventStore>(12,64); auto channel = std::make_shared<BroadcastChannel>(store,1,1); auto events = channel->subscribe();
    {
        AsyncSession session({},channel,1,[](const auto&,const auto&) { return std::string("ok"); }); auto request = session.begin("hello"); drain(session,request);
        check(request->operation()->state() == OperationState::succeeded && request->notifications().finished->status == ObservationStatus::overloaded,
            "observation overload rewrote business success");
        const auto snapshot = request->diagnostics();
        check(snapshot.notifications.started->status == ObservationStatus::accepted &&
            snapshot.notifications.finished->status == ObservationStatus::overloaded && snapshot.failure == TurnFailure::none,
            "turn diagnostics hid observation overload or made it a business failure");
    }
    (void)events->poll();
    std::promise<void> release, entered; const auto gate = release.get_future().share(); auto running = entered.get_future();
    AsyncSession session({},channel,1,
    // 通知后台已经接纳并延后回答，供宿主在真实调用未结束时执行取消。
    // 步骤：报告开始 → 等待放行 → 返回迟到回答。
    [&](const auto&,const auto&) { entered.set_value(); gate.wait(); return std::string("late"); });
    Release cleanup{release}; auto request = session.begin("cancel"); running.wait(); session.close();
    check(request->operation()->state() == OperationState::cancelled && request->exit()->state() == OperationState::pending && session.busy() && !session.closed(),
        "session cancel forged worker exit");
    const auto cancelled = request->diagnostics();
    const auto before_exit = request->snapshot();
    check(before_exit.fact_count == 3 && before_exit.facts[0].kind == RunFactKind::accepted &&
        before_exit.facts[1].kind == RunFactKind::cancel_requested && before_exit.facts[2].kind == RunFactKind::settled &&
        before_exit.facts[2].result == OperationState::cancelled,"cancellation facts lost host ordering");
    check(cancelled.cancel_requested && cancelled.settled && !cancelled.exited && !cancelled.model.finished &&
        cancelled.cancellation == CancelReason::owner_stop && *cancelled.cancel_requested <= *cancelled.settled,
        "cancel diagnostics forged actual exit or lost first cause");
    request->request_cancel(CancelReason::user);
    check(request->diagnostics().cancel_requested == cancelled.cancel_requested,"repeated cancellation rewrote original time");
    rejects<std::logic_error>([&] { (void)session.clear(); }); cleanup.open(); drain(session,request);
    check(session.closed() && !session.message_count() && request->operation()->state() == OperationState::cancelled,"late model reply committed cancelled history");
    const auto exited = request->diagnostics();
    const auto final_facts = request->snapshot();
    check(final_facts.fact_count == 5 && final_facts.facts[3].kind == RunFactKind::model_finished &&
        final_facts.facts[3].model == dfy::model::CallOutcome::returned &&
        final_facts.facts[4].kind == RunFactKind::exited && before_exit.fact_count == 3,
        "late return lost facts, duplicated cancel or rewrote an old snapshot");
    check(exited.attempt == cancelled.attempt && exited.model.request == cancelled.model.request,
        "late completion changed attempt or model request identity");
    check(exited.result == cancelled.result && exited.cancel_requested == cancelled.cancel_requested &&
        exited.settled == cancelled.settled && exited.exited && exited.model.finished &&
        *exited.model.finished >= *exited.settled && *exited.exited >= *exited.model.finished,
        "late return changed cancellation or lost real exit timing");
}
// 验证模型返回后历史提交失败可定位，保留诊断不占用后续模型请求槽。
// 步骤：产生超过历史容量的回答 → 核对提交阶段失败 → 保留旧回合并成功接纳新回合。
void commit_failure() {
    auto channel = std::make_shared<BroadcastChannel>(std::make_shared<EventStore>(8,64),1,8);
    AsyncSession session({},channel,2,[](const auto&,const auto& messages) {
        return messages.back().content == "large" ? std::string(1024*1024,'x') : std::string("ok");
    });
    auto failed = session.begin("large"); drain(session,failed);
    const auto snapshot = failed->diagnostics();
    check(snapshot.failure == TurnFailure::history_commit && snapshot.result == OperationState::failed &&
        snapshot.model.outcome == dfy::model::CallOutcome::returned && snapshot.exited && !session.message_count(),
        "history failure was mislabeled as model failure or committed data");

    auto next = session.begin("small"); drain(session,next);
    check(next->answer() == "ok" && next->turn() == 2 && next->diagnostics().model.request != snapshot.model.request &&
        failed->diagnostics().exited == snapshot.exited,"retained turn diagnostics kept model busy or changed old snapshot");
}
// 验证同序号跨会话不碰撞，拒绝不消耗身份，清空和销毁不改写已交付身份。
// 步骤：分别接纳两会话 → 清空后重新接纳 → 保留值快照并销毁宿主 → 核对原操作和身份。
void identities() {
    std::shared_ptr<TurnRequest> retained;
    AttemptId saved;
    {
        auto channel = std::make_shared<BroadcastChannel>(std::make_shared<EventStore>(16,64),1,8);
        const dfy::model::Completion reply = [](const auto&,const auto&) { return std::string("ok"); };
        AsyncSession first({},channel,2,reply), second({},channel,1,reply);
        rejects<std::invalid_argument>([&] { (void)first.begin(" "); });
        retained = first.begin("first"); auto other = second.begin("other");
        const auto result = retained->operation(), exit = retained->exit();
        saved = retained->attempt_id();
        check(retained->run_id().sequence == 1 && other->run_id().sequence == 1 &&
            retained->run_id() != other->run_id(),"session-local run sequence collided or rejection consumed identity");
        drain(first,retained); drain(second,other);
        first.clear(); auto next = first.begin("next"); drain(first,next);
        check(next->run_id() == RunId{first.id(),2} && next->step_id().sequence == 1 &&
            next->attempt_id().sequence == 1 && retained->attempt_id() == saved,
            "clear reset run identity or a new run inherited previous step identity");
        check(retained->operation() == result && retained->exit() == exit,
            "identity introduced replacement operation state");
    }
    check(retained->diagnostics().attempt == saved && retained->answer() == "ok" &&
        retained->exit()->state() == OperationState::succeeded,"host destruction changed retained attempt");
}
// 验证运行事实独立于通知，查询不推进模型，固定槽位不额外持有结果。
// 步骤：使通知无法接纳 → 只读查询未结算回合 → 推进并核对连续修订 → 回收后复用结果槽。
void facts() {
    auto channel = std::make_shared<BroadcastChannel>(std::make_shared<EventStore>(4,64),1,1);
    auto subscription = channel->subscribe();
    AsyncSession session({},channel,1,[](const auto&,const auto&) { return std::string("ok"); });
    auto request = session.begin("one");
    const auto first = request->snapshot();
    check(first.fact_count == 1 && first.facts[0].revision == 1 &&
        first.diagnostics.result == OperationState::pending && request->snapshot().fact_count == 1,
        "query advanced the model or appended facts");
    drain(session,request); const auto final = request->snapshot();
    check(final.fact_count == 4 && final.facts[1].kind == RunFactKind::model_finished &&
        final.facts[2].kind == RunFactKind::settled && final.facts[2].result == OperationState::succeeded &&
        final.facts[3].kind == RunFactKind::exited &&
        final.diagnostics.notifications.finished->status == ObservationStatus::overloaded,
        "notification overload discarded authoritative run facts");
    for (std::size_t i = 0; i < final.fact_count; ++i) {
        check(final.facts[i].revision == i+1,"run fact revisions are not contiguous");
        if (i) check(final.facts[i-1].observed <= final.facts[i].observed,"host observation ordering changed");
    }
    session.advance(); check(request->snapshot().fact_count == final.fact_count,"repeated advance added facts");
    rejects<std::length_error>([&] { (void)session.begin("retained"); });
    request.reset(); auto next = session.begin("two"); drain(session,next);
    check(next->snapshot().fact_count == 4 && final.diagnostics.turn == 1,"value snapshot retained a result slot");
}
// 验证结构化后端仍走原会话流程，截断或未授权工具响应不能冒充最终文本。
// 步骤：完成文本问答 → 拒绝截断与工具响应 → 核对原历史及调用次数。
void structured_backend() {
    auto channel = std::make_shared<BroadcastChannel>(std::make_shared<EventStore>(8,64),1,8);
    unsigned calls = 0;
    AsyncSession session({},channel,dfy::model::Backend{[&](const auto&,const dfy::model::Request& request) {
        using namespace dfy::model; ++calls;
        if (request.messages.back().content == "length") return Response{{Role::assistant,"partial"},FinishReason::length};
        if (request.messages.back().content == "tools") return Response{{Role::assistant,"",{{"id","read","{}"}}},FinishReason::tool_calls};
        return Response{{Role::assistant,"done"}};
    }},2);
    auto first = session.begin("plain"); drain(session,first);
    check(first->answer() == "done" && session.message_count() == 2,"structured backend lost text compatibility");
    for (const auto* input : {"length","tools"}) {
        auto request = session.begin(input); drain(session,request);
        check(request->operation()->state() == OperationState::failed && request->diagnostics().failure == TurnFailure::model_response &&
            request->diagnostics().model.outcome == dfy::model::CallOutcome::returned && session.message_count() == 2,
            "nonfinal response was committed or mislabeled as history failure");
    }
    check(calls == 3,"structured session duplicated model calls");
}
// 验证正式会话使用上下文事务后，拒绝和提交失败不污染历史，清空不修改已交付结果。
// 步骤：提交一次回答 → 拒绝非法输入及超量回答 → 清空后重新请求 → 核对历史和保留结果。
void context_transactions() {
    auto channel = std::make_shared<BroadcastChannel>(std::make_shared<EventStore>(8,64),1,8);
    std::vector<dfy::model::Message> received; unsigned calls = 0;
    AsyncSession session({},channel,3,[&](const auto&,const auto& messages) {
        ++calls; received = messages;
        return messages.back().content == "overflow" ? std::string(1024*1024,'x') : std::string("ok");
    });
    auto first = session.begin("first"); drain(session,first);
    rejects<std::invalid_argument>([&] { (void)session.begin(std::string(1,static_cast<char>(0xff))); });
    check(calls == 1 && session.message_count() == 2,"invalid context reached backend or changed history");
    auto failed = session.begin("overflow"); drain(session,failed);
    check(failed->turn() == 2 && failed->diagnostics().failure == TurnFailure::history_commit && session.message_count() == 2,
        "failed context commit consumed extra identity or replaced history");
    session.clear(); auto next = session.begin("fresh"); drain(session,next);
    check(calls == 3 && received.size() == 1 && received[0].content == "fresh" && session.message_count() == 2 &&
        first->answer() == "ok" && failed->operation()->state() == OperationState::failed,
        "clear retained prior context or rewrote delivered outcomes");
}
}
// 验收异步会话的历史事务、通知隔离和取消退出，模型使用可控后台替身。
// 步骤：验证上下文及事件 → 验证背压和关闭 → 验证失败诊断、身份与事实 → 报告结果。
int main() {
    try { history(); isolation(); commit_failure(); identities(); facts(); structured_backend(); context_transactions(); std::cout << "Async session checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
