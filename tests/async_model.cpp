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
    AsyncClient client(1,[&](const Config&,const std::vector<Message>& messages) {
        entered.set_value(std::this_thread::get_id()); release.wait(); return messages.back().content;
    });
    Release cleanup{signal}; auto request = client.submit({},{{Role::user,"late answer"}});
    check(worker.get() != owner,"model ran on host thread");
    OwnerScheduler scheduler(1); const auto id = scheduler.add_owner(1,1); bool ran = false;
    scheduler.post(id,[&] { ran = true; }); scheduler.run_one();
    request->request_cancel(CancelReason::user); client.advance();
    check(ran && request->operation()->state() == OperationState::cancelled && request->exit()->state() == OperationState::pending,
        "model waiting blocked scheduler or cancel forged exit");
    rejects<std::length_error>([&] { (void)client.submit({},{{Role::user,"blocked"}}); });
    client.close(); check(!client.closed(),"model close released active thread");
    cleanup.open(); drain(client,request);
    check(client.closed() && client.size() == 1 && request->operation()->cancellation_reason() == CancelReason::user,"late success changed cancel or released retained slot");
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
    rejects<std::length_error>([&] { (void)client.submit({},{{Role::user,"held"}}); });
    request.reset(); client.collect(); request = client.submit({},{{Role::user,"fail"}}); drain(client,request);
    check(request->operation()->state() == OperationState::failed,"backend error became success");
    rejects<std::runtime_error>([&] { (void)request->answer(); }); request.reset(); client.collect();
    rejects<std::length_error>([&] { (void)client.submit({},{{Role::user,std::string(1024*1024+1,'x')}}); });
    request = client.submit({},{{Role::user,"orphan"}}); const auto exit = request->exit(); request.reset(); client.close();
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (!client.closed() && std::chrono::steady_clock::now() < deadline) { client.advance(); std::this_thread::yield(); }
    client.collect(); check(client.closed() && !client.size() && exit->state() == OperationState::succeeded,"client release lost worker responsibility");
}
}
// 验收真实后台线程与宿主唯一结算，所有模型结果使用可控离线替身。
// 步骤：验证取消及在途责任 → 验证结果和异常 → 报告结果。
int main() {
    try { cancellation(); outcomes(); std::cout << "Async model checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
