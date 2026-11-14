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
    auto b = session.begin("fail"); drain(session,b);
    check(b->operation()->state() == OperationState::failed && session.message_count() == 2,"model failure polluted history");
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
    rejects<std::logic_error>([&] { (void)session.clear(); }); cleanup.open(); drain(session,request);
    check(session.closed() && !session.message_count() && request->operation()->state() == OperationState::cancelled,"late model reply committed cancelled history");
}
}
// 验收异步会话的历史事务、通知隔离和取消退出，模型使用可控后台替身。
// 步骤：验证上下文及事件 → 验证背压和关闭 → 报告结果。
int main() {
    try { history(); isolation(); std::cout << "Async session checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
