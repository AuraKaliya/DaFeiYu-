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

enum class InputBoundary { model, admitted, confirmation };

// 核对新原话送模前的旧变更、独立读取及原批准，随后允许模型依新指令再次选择。
// 步骤：提出读写混合批次 → 在指定边界追加原话 → 检查旧回执和新请求 → 核对实际动作次数。
void pending_changes(InputBoundary boundary) {
    unsigned reads = 0;
    std::vector<std::string> changes;
    ToolCatalog catalog;
    const std::string schema = R"({"type":"object","properties":{},"additionalProperties":false})";
    catalog.add({"fixture/read",1,1,{"read","读取当前库存",schema}},[&](const auto&) {
        ++reads; return completed_tool({true,ToolEffect::not_applied,"eight",ToolError::none});
    });
    catalog.add({"fixture/change",1,1,{"change","执行已授权的交付",schema},true,false},[&](const auto& call) {
        changes.push_back(call.call.id); return completed_tool({true,ToolEffect::applied,"delivered",ToolError::none});
    });
    std::vector<Request> received;
    Session session({},channel(),Backend{[&](const auto&,const Request& input) -> Response {
        received.push_back(input);
        if (received.size()==1) return {{Role::assistant,"",{{"stock","read","{}"},{"old","change","{}"}}},FinishReason::tool_calls};
        if (received.size()==2 && boundary!=InputBoundary::confirmation)
            return {{Role::assistant,"",{{"revised","change","{}"}}},FinishReason::tool_calls};
        return {{Role::assistant,"已核对"}};
    }});
    ConfirmationCaller caller;
    RunOptions options;
    options.accept_user_input = true;
    options.tools = catalog.snapshot(std::vector<std::string>{"fixture/read","fixture/change"},true);
    options.confirmation_caller = caller;
    options.policy.confirm_changes = boundary==InputBoundary::confirmation;
    const auto request = session.begin("先查库存，交给甲",options);
    if (boundary!=InputBoundary::model) until(session,[&] { return !request->tool_steps().empty(); });
    check(session.append_input(request->run_id(),"改交给乙").state==InputAdmission::accepted,"correction not admitted");
    if (boundary==InputBoundary::confirmation) {
        const auto pending = request->confirmations().at(0);
        bool refused = false;
        try { session.decide(caller,{1,pending.id,pending.revision,pending.target,true}); }
        catch (const std::invalid_argument&) { refused = true; }
        check(refused && request->confirmations().front().state==ConfirmationState::invalidated,"stale approval survived correction");
    }
    until(session,[&] { return !session.busy(); });

    check(request->operation()->state()==OperationState::succeeded && request->inputs().at(0).model_step==2 &&
        received.size()==(boundary==InputBoundary::confirmation ? 2u : 3u),"correction lost before fresh decision");
    const auto& context = received[1].messages;
    check(context.size()==5 && context[2].tool_call_id=="stock" && context[2].content.find("eight")!=std::string::npos &&
        context[3].tool_call_id=="old" && context[3].content.find("effect=not_applied")!=std::string::npos &&
        context[3].content.find("user_input_pending")!=std::string::npos &&
        context.back().role==Role::user && context.back().content=="改交给乙","next request lost evidence or accepted a stale effect");
    check(reads==1 && changes==(boundary==InputBoundary::confirmation ? std::vector<std::string>{} : std::vector<std::string>{"revised"}),
        "stale write started or fresh decision lost its grant");
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
        same_turn();
        for (auto boundary : {InputBoundary::model,InputBoundary::admitted,InputBoundary::confirmation}) pending_changes(boundary);
        undelivered(InputState::limit); undelivered(InputState::deadline); undelivered(InputState::cancelled); input_budget();
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
