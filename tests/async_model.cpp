#include <dafeiyu/async_model.hpp>
#include <dafeiyu/owner_scheduler.hpp>
#include <iostream>
#include <thread>
#include <atomic>
namespace {
using namespace dfy::runtime;
using namespace dfy::model;
// 在后台调用、取消裁定或实际退出边界违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 核对明确错误类别，保留无关异常供入口报告。
template<class Error, class Action>
void rejects(Action action) { try { action(); } catch (const Error&) { return; } throw std::runtime_error("async model accepted invalid action"); }
struct Release {
    std::promise<void>& promise; bool released = false;
    // 即使断言失败也解除替身阻塞，避免测试析构等待一个永不退出的线程。
    ~Release() { if (!released) promise.set_value(); }
    // 只解除一次受控后台等待。
    void open() { promise.set_value(); released = true; }
};
// 在有界时间内推进真实工作线程交还，不用睡眠模拟已经退出。
void drain(AsyncClient& client, const std::shared_ptr<ModelRequest>& request) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (request->exit()->state() == OperationState::pending && std::chrono::steady_clock::now() < deadline) { client.advance(); std::this_thread::yield(); }
    check(request->exit()->state() == OperationState::succeeded,"async worker did not exit");
}
// 验证模型等待期间宿主可继续调度，业务取消不能伪造线程退出或释放请求容量。
// 步骤：阻塞真实后台替身 → 独立调度并请求取消 → 核对忙碌容量 → 解除工作并丢弃迟到成功。
void cancellation() {
    std::promise<void> signal; const auto release = signal.get_future().share(); std::promise<std::thread::id> entered;
    auto worker = entered.get_future(); const auto owner = std::this_thread::get_id();
    AsyncClient client(1,
    // 在真实后台报告线程身份后等待测试放行，再产生可被取消裁定丢弃的回答。
    // 步骤：通知已进入后台 → 等待可控门 → 返回完整文本。
    [&](const Config&,const std::vector<Message>& messages) {
        entered.set_value(std::this_thread::get_id()); release.wait(); return messages.back().content;
    });
    Release cleanup{signal}; auto request = client.submit({},{{Role::user,"late answer"}});
    check(worker.get() != owner,"model ran on host thread");
    const auto running = request->diagnostics();
    check(running.request && running.started && running.submitted <= *running.started && !running.finished &&
        running.outcome == CallOutcome::pending,"live model diagnostics invented completion");
    OwnerScheduler scheduler(1); const auto id = scheduler.add_owner(1,1); bool ran = false;
    scheduler.post(id,[&] { ran = true; }); scheduler.run_one();
    request->request_cancel(CancelReason::user); client.advance();
    check(ran && request->operation()->state() == OperationState::cancelled && request->exit()->state() == OperationState::pending,
        "model waiting blocked scheduler or cancel forged exit");
    check(!request->diagnostics().finished,"business cancellation forged call finish time");
    rejects<std::length_error>([&] { (void)client.submit({},{{Role::user,"blocked"}}); });
    client.close(); check(!client.closed(),"model close released active thread");
    cleanup.open(); drain(client,request);
    check(client.closed() && client.size() == 1 && request->operation()->cancellation_reason() == CancelReason::user,"late success changed cancel or released retained slot");
    const auto finished = request->diagnostics();
    check(finished.request == running.request && finished.started == running.started && finished.finished &&
        *finished.finished >= *finished.started && finished.outcome == CallOutcome::returned,
        "late call return lost timing or replaced cancellation");
    rejects<std::runtime_error>([&] { (void)request->answer(); }); request.reset(); client.collect(); check(!client.size(),"model request leaked after last client");
}
// 验证完整文本与异常传播、客户端释放后的宿主责任和失败前的输入限制。
// 步骤：拒绝非法输入 → 返回完整回答并检查保留容量 → 读取失败 → 提前释放客户端 → 宿主关闭回收。
void outcomes() {
    std::atomic<unsigned> calls{0};
    AsyncClient client(1,[&](const Config&,const std::vector<Message>& messages) {
        ++calls;
        if (messages.back().content == "fail") throw std::runtime_error("model: controlled failure");
        return messages.back().content;
    });
    rejects<std::invalid_argument>([&] { (void)client.submit({},{{Role::user,""},{Role::assistant,""}}); });
    rejects<std::invalid_argument>([&] { (void)client.submit({},{{static_cast<Role>(99),"invalid"}}); });
    check(!calls.load() && !client.size(),"invalid message metadata entered backend or consumed a slot");
    auto request = client.submit({},{{Role::user,"完整回答"}}); drain(client,request);
    check(request->answer() == "完整回答" && request->operation()->state() == OperationState::succeeded,"complete text lost");
    const auto success = request->diagnostics();
    check(success.started && success.finished && success.outcome == CallOutcome::returned,"successful call lost diagnostics");
    rejects<std::length_error>([&] { (void)client.submit({},{{Role::user,"held"}}); });
    request.reset(); client.collect(); request = client.submit({},{{Role::user,"fail"}}); drain(client,request);
    check(request->operation()->state() == OperationState::failed,"backend error became success");
    const auto failure = request->diagnostics();
    check(failure.request != success.request && failure.started && failure.finished && failure.outcome == CallOutcome::threw &&
        failure.submitted <= *failure.started && *failure.started <= *failure.finished,"failed call lost independent timing");
    rejects<std::runtime_error>([&] { (void)request->answer(); }); request.reset(); client.collect();
    rejects<std::length_error>([&] { (void)client.submit({},{{Role::user,std::string(1024*1024+1,'x')}}); });
    request = client.submit({},{{Role::user,"orphan"}}); const auto exit = request->exit(); request.reset(); client.close();
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (!client.closed() && std::chrono::steady_clock::now() < deadline) { client.advance(); std::this_thread::yield(); }
    client.collect(); check(client.closed() && !client.size() && exit->state() == OperationState::succeeded,"client release lost worker responsibility");
}
// 验证诊断读取不代替宿主结算，后台返回文本也不代表输出校验成功。
// 步骤：等待后台发布返回事实 → 只读快照核对 Pending → 推进并验证空文本失败 → 保留快照并回收槽位。
void diagnostic_boundary() {
    AsyncClient client(1,[](const auto&,const auto&) { return std::string(); });
    auto request = client.submit({},{{Role::user,"empty reply"}});
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (!request->diagnostics().finished && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    const auto snapshot = request->diagnostics();
    check(snapshot.finished && snapshot.outcome == CallOutcome::returned && request->operation()->state() == OperationState::pending &&
        request->exit()->state() == OperationState::pending,"diagnostic read settled host result or exit");

    drain(client,request);
    check(request->operation()->state() == OperationState::failed && request->diagnostics().finished == snapshot.finished,
        "invalid output lost actual call return or changed finish time");
    request.reset(); client.collect();
    check(!client.size() && snapshot.request,"diagnostic snapshot retained a model slot");
}
// 验证结构化后端复用原操作生命周期，按预设调用结果推进且不回退真实服务。
// 步骤：拒绝空后端与错误选项 → 返回固定工具调用 → 回填固定结果 → 取消后丢弃迟到响应。
void structured() {
    rejects<std::invalid_argument>([] { AsyncClient invalid(1,Backend{}); });
    const Request input{{{Role::system,"rules"},{Role::user,"goal"}},{{"read","read","{}"}},32};
    AsyncClient text(1,[](const auto&,const auto&) { return std::string("text"); });
    rejects<std::invalid_argument>([&] { (void)text.submit_request({},input); });

    unsigned calls = 0;
    AsyncClient client(1,Backend{[&](const Config&,const Request& request) {
        ++calls;
        if (calls == 1) {
            check(request.tools.size() == 1 && request.max_output_tokens == 32u,"structured request metadata lost");
            return Response{{Role::assistant,"",{{"call","read","{}"}}},FinishReason::tool_calls};
        }
        check(request.messages.back().tool_call_id == "call","tool result did not reach backend");
        return Response{{Role::assistant,"done"}};
    }});
    auto first = client.submit_request({},input);
    rejects<std::logic_error>([&] { (void)first->response(); }); drain(client,first);
    const auto reply = first->response();
    check(reply.message.tool_calls.size() == 1 && first->operation()->state() == OperationState::succeeded,
        "structured response was treated as empty text");
    rejects<std::logic_error>([&] { (void)first->answer(); }); first.reset(); client.collect();
    auto next = input; next.messages.push_back(reply.message); next.messages.push_back({Role::tool,"value",{},"call"});
    auto final = client.submit_request({},next); drain(client,final);
    check(final->answer() == "done" && calls == 2,"fixed model sequence did not complete");
    final.reset(); client.collect(); auto cancelled = client.submit_request({},next);
    cancelled->request_cancel(CancelReason::user); drain(client,cancelled);
    check(cancelled->operation()->state() == OperationState::cancelled,"late structured response replaced cancellation");
    rejects<std::runtime_error>([&] { (void)cancelled->response(); });
}
}
// 验收真实后台线程与宿主唯一结算，所有模型结果使用可控离线替身。
// 步骤：验证取消及在途责任 → 验证结果和异常 → 验证诊断与结构化后端 → 报告结果。
int main() {
    try { cancellation(); outcomes(); diagnostic_boundary(); structured(); std::cout << "Async model checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
