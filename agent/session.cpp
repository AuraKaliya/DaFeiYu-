#include <dafeiyu/session.hpp>
#include <dafeiyu/run_loop.hpp>
#include <limits>

namespace dfy::agent {
using namespace runtime;
namespace {
// 将原文本调用包装为明确的文本后端，不允许静默丢弃工具字段。
// 步骤：选取明确调用或内置文本 Provider → 包装完整响应并标记文本能力。
model::Backend text_backend(model::Completion completion) {
    if (!completion) completion = [](const auto& config,const auto& messages) { return model::complete(config,messages); };
    return {[completion = std::move(completion)](const model::Config& config,const model::Request& request) {
        return model::Response{{model::Role::assistant,completion(config,request.messages)}};
    },false};
}
// 定义跨插件共享的会话事实结构，不包含供应商参数或认证信息。
std::shared_ptr<const StructDescriptor> event_descriptor() {
    const FieldDescriptor fields[] = {{"kind",ValueKind::string,32},{"session",ValueKind::uint64,0},
        {"turn",ValueKind::uint64,0},{"messages",ValueKind::uint64,0}};
    return std::make_shared<const StructDescriptor>(StructDescriptor::create("dfy.session.event",1,fields,{4,64}));
}
}
// 保留完整回合结果的借用边界，失败和取消不能冒充空回答。
const std::string& TurnRequest::answer() const {
    if (operation()->state() == OperationState::failed) std::rethrow_exception(error_);
    if (operation()->state() == OperationState::cancelled) throw std::runtime_error("session: cancelled");
    if (operation()->state() != OperationState::succeeded) throw std::logic_error("session: answer pending");
    return answer_;
}
// 记录首次有效取消的时刻，重复请求或已结算回合不改写诊断。
// 步骤：交由原操作登记取消 → 仅在首次接纳时固定时间 → 返回原取消回执。
CancelDisposition TurnRequest::request_cancel(CancelReason reason) {
    const auto disposition = result_.request_cancel(reason);
    if (disposition == CancelDisposition::recorded) {
        cancel_requested_ = std::chrono::steady_clock::now(); record_fact(RunFactKind::cancel_requested);
    }
    return disposition;
}
// 用原操作状态和单次记录的时间形成回合视图，不维护第二套业务状态。
// 步骤：复制身份与时间 → 读取业务及退出状态 → 关联模型事实和通知接纳回执。
TurnDiagnostics TurnRequest::diagnostics() const noexcept {
    TurnDiagnostics result;
    result.session = run_id().session; result.turn = turn(); result.attempt = attempt_; result.accepted = accepted_; result.origin = origin_;
    result.cancel_requested = cancel_requested_; result.settled = settled_; result.exited = exited_;
    result.result = operation()->state(); result.exit = exit()->state();
    result.cancellation = operation()->cancellation_reason(); result.failure = failure_;
    result.model = model_ ? model_->diagnostics() : completed_call_;
    result.model_result = model_ ? model_->operation()->state() : completed_model_result_;
    result.model_exit = model_ ? model_->exit()->state() : completed_model_exit_;
    result.notifications = notifications_;
    result.model_requests = model_requests_; result.tool_requests = tool_requests_; result.stop_reason = stop_reason_;
    if (confirmations_) { result.confirmation_requests = confirmations_->size(); result.waiting_confirmations = confirmations_->waiting(); }
    return result;
}
// 在固定槽位记录首次发生的生命周期事实，重复推进不追加重复项。
// 步骤：定位本类事实槽 → 忽略已记录事实 → 复制原状态并签发宿主顺序修订。
void TurnRequest::record_fact(RunFactKind kind) noexcept {
    auto& fact = facts_[static_cast<std::size_t>(kind)];
    if (fact.revision) return;
    const auto current = diagnostics();
    fact = {++fact_count_,kind,std::chrono::steady_clock::now(),current.result,current.cancellation,
        current.model.outcome,current.failure,current.attempt,current.model.request};
}
// 复制诊断并按首次记录顺序排列事实，不触发后台结果接收或事件消费。
// 步骤：取得诊断值副本 → 按修订号复制已记录项 → 返回固定容量快照。
RunSnapshot TurnRequest::snapshot() const noexcept {
    RunSnapshot result; result.diagnostics = diagnostics(); result.fact_count = fact_count_;
    for (const auto& fact : facts_) if (fact.revision) result.facts[fact.revision-1] = fact;
    return result;
}
// 合并批次的只读值投影，查询不推进工作或延长代码寿命。
std::vector<BatchToolSnapshot> TurnRequest::tool_steps() const {
    std::vector<BatchToolSnapshot> result;
    for (const auto& batch : batches_) { auto values = batch->snapshot(); result.insert(result.end(),values.begin(),values.end()); }
    return result;
}
// 只复制确认值，不推进模型、工具或控制决定。
std::vector<ConfirmationSnapshot> TurnRequest::confirmations() const { return confirmations_ ? confirmations_->snapshot() : std::vector<ConfirmationSnapshot>{}; }
// 完整准备会话数据和有界模型客户端，不创建待执行问答。
Session::Session(model::Config config, std::shared_ptr<events::BroadcastChannel> events,
    std::size_t result_capacity, model::Completion completion)
    : Session(std::move(config),std::move(events),text_backend(std::move(completion)),result_capacity) {}
// 完整准备结构化后端，状态仍由原会话的结果和退出操作负责。
Session::Session(model::Config config, std::shared_ptr<events::BroadcastChannel> events, model::Backend backend, std::size_t result_capacity)
    : config_(std::move(config)), events_(std::move(events)), descriptor_(event_descriptor()), id_(next_domain()),
      records_(result_capacity), backend_(std::move(backend)) {
    if (!events_ || !result_capacity || !backend_.complete) throw std::invalid_argument("session: channel, backend and result capacity required");
}
// 兜底关闭时先保留所有回合责任，实际模型退出后再结算回合退出。
// 步骤：关闭接纳 → 等待原模型退出 → 推进工具收尾 → 将未退出责任交给保留链。
Session::~Session() { close(); if (active_) active_->wait_model(); advance(); active_.reset(); }
// 以不可变事件传递会话事实，观察接纳或分配失败只形成独立回执。
// 步骤：构造类型化字段 → 创建宿主载荷 → 接纳观察通知或返回失败回执。
events::ObservationReceipt Session::notify(std::string_view kind, std::uint64_t turn) noexcept {
    try {
        const NamedValue values[] = {{"kind",Value::from_string(kind,32)},{"session",Value::from_uint64(id_)},
            {"turn",Value::from_uint64(turn)},{"messages",Value::from_uint64(history_->size())}};
        return events::observe(*events_,std::make_shared<const StructValue>(StructValue::create(descriptor_,values,64)));
    } catch (...) { return {events::ObservationStatus::failed,{}}; }
}
// 原入口沿用无工具选项，仍通过同一正式运行协调器执行。
std::shared_ptr<TurnRequest> Session::begin(const std::string& prompt) { return begin(prompt,{}); }
// 完整准备和控制检查成功后才接纳，拒绝不消耗回合号或改变已提交上下文。
// 步骤：检查入口与输入 → 包装关闭边界 → 准备有界协调器 → 固定接纳并通知开始。
std::shared_ptr<TurnRequest> Session::begin(const std::string& prompt, RunOptions options) {
    if (!*accepting_ || active_ || working_) throw std::logic_error("session: busy, closed or reentrant");
    collect();
    if (prompt.empty() || prompt.find_first_not_of(" \t\r\n") == std::string::npos || prompt.size() > 1024*1024)
        throw std::invalid_argument("session: nonblank prompt within 1 MiB required");
    if (turn_ == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("session: turn exhausted");
    // 用独立会话门保护规则回调，关闭发生在回调内时仍能阻止新的受控调用。
    // 步骤：调用明确规则 → 重查会话门 → 返回原决定或关闭拒绝。
    options.extension = [gate = accepting_,rule = std::move(options.extension)](ControlPoint point,const PolicyContext& context) {
        const auto decision = rule ? rule(point,context) : PolicyDecision{PolicyVerdict::allow,PolicyReason::none};
        return *gate ? decision : PolicyDecision{PolicyVerdict::deny,PolicyReason::cancelled};
    };
    working_ = true;
    try {
        for (auto& slot : records_) if (!slot) {
            auto loop = std::make_unique<RunLoop>(RunId{id_,turn_+1},config_,backend_,history_,prompt,std::move(options));
            auto record = loop->request(); active_ = std::move(loop); slot = record; ++turn_;
            record->notifications_.started = notify("turn_started",turn_); working_ = false; return record;
        }
    } catch (...) { working_ = false; throw; }
    working_ = false; throw std::length_error("session: retained turn capacity exhausted");
}
// 路由可信控制命令，已退出结果只复用原回执，不重建执行器或恢复终态。
// 步骤：保护控制入口 → 按完整运行身份定位 → 活动运行重查或终态回执复用 → 归还入口。
ConfirmationReceipt Session::decide(const ConfirmationCaller& caller, const ConfirmationCommand& command) {
    if (working_) throw std::logic_error("session: reentrant decision");
    working_ = true;
    try {
        for (const auto& record : records_) if (record && record->run_id() == command.expected.attempt.step.run && record->confirmations_) {
            const auto receipt = active_ && active_->request() == record ? active_->decide(caller,command) :
                record->confirmations_->decide(caller,command,ToolTime::max());
            working_ = false; return receipt;
        }
    } catch (...) { working_ = false; throw; }
    working_ = false; throw std::invalid_argument("session: confirmation run unavailable");
}
// 推进统一运行并同步完整历史，业务终态通知和真实退出分别处理。
// 步骤：保护宿主入口 → 推进一个运行阶段 → 采纳已提交上下文 → 发布唯一终态通知 → 退出后归还活动槽。
void Session::advance() {
    if (working_) throw std::logic_error("session: reentrant advance");
    if (!active_) return;
    working_ = true;
    try {
        active_->advance(); history_ = active_->history(); const auto record = active_->request();
        const auto state = record->operation()->state();
        if (state != OperationState::pending && !record->notifications_.finished)
            record->notifications_.finished = notify(state == OperationState::succeeded ? "turn_completed" :
                state == OperationState::cancelled ? "turn_cancelled" : "turn_failed",record->turn());
        if (record->exit()->state() != OperationState::pending) active_.reset();
    } catch (...) { working_ = false; throw; }
    working_ = false;
}
// 清空只允许在实际空闲时执行，不改变回合序号，也不调用模型。
events::ObservationReceipt Session::clear() {
    if (!*accepting_ || active_ || working_) throw std::logic_error("session: busy or closed");
    history_ = std::make_shared<const MessageHistory>(); return notify("session_cleared",0);
}
// 停止新运行并请求当前工作退出，回调内关闭只登记取消而不重入推进。
// 步骤：关闭接纳 → 登记当前取消 → 非重入时推进收尾。
void Session::close() {
    *accepting_ = false; if (active_) active_->request()->request_cancel(CancelReason::owner_stop);
    if (!working_) advance();
}
// 返回原协调器保留的真实收尾数量。
std::size_t Session::retained() noexcept { return RunLoop::retained(); }
// 复用同一协调器收尾，不创建第二条执行路径。
std::size_t Session::retry_retained() { return RunLoop::retry_retained(); }
// 释放没有客户端的终态结果，尚未实际退出的回合始终保留。
void Session::collect() noexcept {
    for (auto& record : records_) if (record && record->exit()->state() != OperationState::pending && record.use_count() == 1) record.reset();
}
}
