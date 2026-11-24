#include <dafeiyu/session.hpp>
#include <iostream>
#include <semaphore>
#include <thread>

namespace {
using namespace dfy::agent;
using namespace dfy::model;
using dfy::runtime::OperationState;
using namespace std::chrono_literals;

// 将不符合原消息、真实请求或回合生命周期的结果报告为验收失败。
void check(bool condition, const char* detail) { if (!condition) throw std::runtime_error(detail); }

// 为每个场景建立独立会话所需的有界事件通道。
auto channel() { return std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(16,64),1,8); }

// 只在原宿主线程推进，按真实状态等待指定边界。
template<class Condition>
void until(Session& session, Condition done) {
    const auto deadline = std::chrono::steady_clock::now()+3s;
    while (!done() && std::chrono::steady_clock::now()<deadline) { session.advance(); std::this_thread::yield(); }
    check(done(),"turn input fixture timed out");
}

// 固定前后两次真实请求，验证追加顺序、模型步骤、原回合和最终历史。
// 步骤：保持首个请求已发 → 追加两句 → 放行原响应 → 核对第二次输入及最终结算。
void same_turn() {
    std::binary_semaphore first(0), second(0), release(0);
    std::vector<Request> received;
    Backend backend{[&](const auto&,const Request& input) -> Response {
        received.push_back(input);
        if (received.size()==1) { first.release(); return {{Role::assistant,"阶段文本"}}; }
        second.release();
        if (!release.try_acquire_for(3s)) throw std::runtime_error("second call not released");
        return {{Role::assistant,"合并后的最终回答"}};
    }};
    Session session({},channel(),std::move(backend));
    RunOptions options; options.accept_user_input = true;
    const auto request = session.begin("最初原话",options);
    const auto run = request->run_id();
    check(first.try_acquire_for(2s),"first call did not receive its input");
    check(session.append_input({run.session,run.sequence+1},"错误回合").state==InputAdmission::closed,"stale turn accepted input");
    const auto a = session.append_input(run,"补充甲"), b = session.append_input(run,"补充乙");
    check(a.message==1 && b.message==2 && request->inputs().size()==2 && request->inputs()[0].state==InputState::waiting &&
        !request->inputs()[0].model_step && received.front().messages.size()==1,"admission altered submitted first call");
    until(session,[&] { return request->inputs().back().state==InputState::submitted; });
    check(second.try_acquire_for(2s) && received.size()==2 && received.back().messages.size()==4,"second call missing actual inputs");
    const auto& messages = received.back().messages;
    check(messages[0].content=="最初原话" && messages[1].role==Role::assistant && messages[1].content=="阶段文本" &&
        messages[2].role==Role::user && messages[2].content=="补充甲" && messages[3].role==Role::user && messages[3].content=="补充乙" &&
        request->inputs()[0].model_step==2 && request->inputs()[1].model_step==2 &&
        request->operation()->state()==OperationState::pending,"supplements were duplicated, reordered, or completed the turn early");
    release.release();
    until(session,[&] { return !session.busy(); });
    check(request->run_id()==run && request->answer()=="合并后的最终回答" && session.message_count()==5 &&
        session.append_input(run,"晚到").state==InputAdmission::closed,"closed turn accepted input or exposed a partial answer");
}

// 在不同收尾条件下逐条核对未带入回执，不把未提交的上下文留给下一轮。
// 步骤：设置原请求限制 → 接纳或拒绝补充 → 触发限制或取消 → 核对模型调用数和历史。
void undelivered(InputState expected) {
    Session session({},channel(),Backend{[](const auto&,const auto&) -> Response { return {{Role::assistant,"原阶段"}}; }});
    RunOptions options; options.accept_user_input = true;
    if (expected==InputState::limit) options.policy.model_limit = 1;
    auto now = ToolTime{};
    options.now = [&] { return now; };
    const auto request = session.begin("原话",options);
    check(session.append_input(request->run_id(),"  ").state==InputAdmission::rejected,"blank input accepted");
    for (unsigned i=0;i<8;++i) check(session.append_input(request->run_id(),"补充").message==i+1,"valid input not admitted");
    check(session.append_input(request->run_id(),"第九条").state==InputAdmission::full,"input capacity exceeded");
    if (expected==InputState::cancelled) request->request_cancel(dfy::runtime::CancelReason::user);
    if (expected==InputState::deadline) now += options.timeout;
    until(session,[&] { return !session.busy(); });
    for (const auto& input : request->inputs()) check(input.state==expected && !input.model_step,"wrong undelivered reason or invented request");
    check(request->diagnostics().model_requests==1 && !session.message_count(),"failed delivery created a model request or committed draft history");
}

// 核对入口授权、单条容量，以及组合后超出实际请求预算时的未带入原因。
// 步骤：普通入口拒绝 → 授权收件并填满字节预算 → 封闭前核对真实请求大小。
void input_budget() {
    Session session({},channel(),Backend{[](const auto&,const auto&) -> Response { return {{Role::assistant,"固定原答"}}; }});
    const auto plain = session.begin("普通入口");
    check(session.append_input(plain->run_id(),"无入口").state==InputAdmission::unavailable,"unsolicited input enabled");
    until(session,[&] { return !session.busy(); });
    RunOptions options; options.accept_user_input = true;
    const auto request = session.begin("第二轮",options);
    const auto run = request->run_id();
    check(session.append_input(run,std::string(1024*1024+1,'x')).state==InputAdmission::rejected &&
        session.append_input(run,std::string(1024*1024,'x')).state==InputAdmission::accepted &&
        session.append_input(run,"多出一个字").state==InputAdmission::full,"supplement byte budget was not enforced");
    until(session,[&] { return !session.busy(); });
    check(request->inputs().front().state==InputState::limit && !request->inputs().front().model_step &&
        request->diagnostics().model_requests==1 && session.message_count()==2,"oversized assembled request changed history or delivery");
}
}

// 运行本轮追加入件和未带入结算的离线场景。
int main() {
    try {
        same_turn(); undelivered(InputState::limit); undelivered(InputState::deadline); undelivered(InputState::cancelled); input_budget();
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
