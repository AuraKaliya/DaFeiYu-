#include <dafeiyu/session.hpp>
#include <limits>

namespace dfy::agent {
using namespace runtime;
namespace {
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
    result.session = run_id().session; result.turn = turn(); result.attempt = attempt_; result.accepted = accepted_;
    result.cancel_requested = cancel_requested_; result.settled = settled_; result.exited = exited_;
    result.result = operation()->state(); result.exit = exit()->state();
    result.cancellation = operation()->cancellation_reason(); result.failure = failure_;
    result.model = model_ ? model_->diagnostics() : completed_call_;
    result.notifications = notifications_;
    return result;
}
// 在固定槽位记录首次发生的生命周期事实，重复推进不追加重复项。
// 步骤：定位本类事实槽 → 忽略已记录事实 → 复制原状态并签发宿主顺序修订。
void TurnRequest::record_fact(RunFactKind kind) noexcept {
    auto& fact = facts_[static_cast<std::size_t>(kind)];
    if (fact.revision) return;
    const auto current = diagnostics();
    fact = {++fact_count_,kind,std::chrono::steady_clock::now(),current.result,current.cancellation,
        current.model.outcome,current.failure};
}
// 复制诊断并按首次记录顺序排列事实，不触发后台结果接收或事件消费。
// 步骤：取得诊断值副本 → 按修订号复制已记录项 → 返回固定容量快照。
RunSnapshot TurnRequest::snapshot() const noexcept {
    RunSnapshot result; result.diagnostics = diagnostics(); result.fact_count = fact_count_;
    for (const auto& fact : facts_) if (fact.revision) result.facts[fact.revision-1] = fact;
    return result;
}
// 完整准备会话数据和有界模型客户端，不创建待执行问答。
Session::Session(model::Config config, std::shared_ptr<events::BroadcastChannel> events,
    std::size_t result_capacity, model::Completion completion)
    : config_(std::move(config)), events_(std::move(events)), descriptor_(event_descriptor()), id_(next_domain()),
      records_(result_capacity), models_(1,std::move(completion)) {
    if (!events_ || !result_capacity) throw std::invalid_argument("session: event channel and positive result capacity required");
}
// 完整准备结构化后端，状态仍由原会话的结果和退出操作负责。
Session::Session(model::Config config, std::shared_ptr<events::BroadcastChannel> events, model::Backend backend, std::size_t result_capacity)
    : config_(std::move(config)), events_(std::move(events)), descriptor_(event_descriptor()), id_(next_domain()),
      records_(result_capacity), models_(1,std::move(backend)) {
    if (!events_ || !result_capacity) throw std::invalid_argument("session: event channel and positive result capacity required");
}
// 兜底关闭时先保留所有回合责任，实际模型退出后再结算回合退出。
Session::~Session() { close(); models_.wait_closed(); advance(); }
// 以不可变事件传递会话事实，观察接纳或分配失败只形成独立回执。
// 步骤：构造类型化字段 → 创建宿主载荷 → 接纳观察通知或返回失败回执。
events::ObservationReceipt Session::notify(std::string_view kind, std::uint64_t turn) noexcept {
    try {
        const NamedValue values[] = {{"kind",Value::from_string(kind,32)},{"session",Value::from_uint64(id_)},
            {"turn",Value::from_uint64(turn)},{"messages",Value::from_uint64(history_->size())}};
        return events::observe(*events_,std::make_shared<const StructValue>(StructValue::create(descriptor_,values,64)));
    } catch (...) { return {events::ObservationStatus::failed,{}}; }
}
// 完整准备成功后才接受回合，拒绝不消耗回合号或改变已提交上下文。
// 步骤：检查空闲、输入和结果槽 → 准备待提交历史及回合 → 提交后台模型 → 固定接纳并通知开始。
std::shared_ptr<TurnRequest> Session::begin(const std::string& prompt) {
    collect();
    if (!accepting_ || active_) throw std::logic_error("session: busy or closed");
    if (prompt.empty() || prompt.find_first_not_of(" \t\r\n") == std::string::npos || prompt.size() > 1024*1024)
        throw std::invalid_argument("session: nonblank prompt within 1 MiB required");
    if (turn_ == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("session: turn exhausted");
    for (auto& slot : records_) if (!slot) {
        auto record = std::shared_ptr<TurnRequest>(new TurnRequest({id_,turn_+1}));
        record->context_ = std::make_unique<RunContext>(history_,prompt);
        auto request = models_.submit_request(config_,model::Request{record->context_->messages()});
        record->model_ = std::move(request); active_ = record; slot = record; ++turn_;
        record->accepted_ = std::chrono::steady_clock::now();
        record->record_fact(RunFactKind::accepted);
        record->notifications_.started = notify("turn_started",turn_); return record;
    }
    throw std::length_error("session: retained turn capacity exhausted");
}
// 取消优先于同次推进观察到的模型结果，完整问答准备成功才原子提交历史。
// 步骤：裁定回合取消 → 推进后台结果 → 准备历史并记录结算阶段 → 通知唯一终态 → 保存调用事实并确认退出。
void Session::advance() {
    if (active_ && active_->operation()->state() == OperationState::pending && active_->operation()->cancellation_reason() != CancelReason::none) {
        active_->model_->request_cancel(active_->operation()->cancellation_reason()); active_->result_.settle(OperationState::cancelled);
        active_->settled_ = std::chrono::steady_clock::now();
        active_->record_fact(RunFactKind::settled);
        active_->context_->seal(); active_->notifications_.finished = notify("turn_cancelled",active_->turn());
    }
    models_.advance();
    if (!active_ || active_->model_->exit()->state() == OperationState::pending) return;
    active_->record_fact(RunFactKind::model_finished);
    if (active_->operation()->state() == OperationState::pending) {
        try {
            const auto& response = active_->model_->response();
            if (response.finish != model::FinishReason::stop || !response.message.tool_calls.empty()) {
                active_->failure_ = TurnFailure::model_response;
                throw std::runtime_error("session: final text response required");
            }
            auto answer = response.message.content;
            active_->context_->finish_text(answer); history_ = active_->context_->committed();
            active_->answer_ = std::move(answer); active_->result_.settle(OperationState::succeeded);
        } catch (...) {
            active_->error_ = std::current_exception();
            if (active_->failure_ == TurnFailure::none)
                active_->failure_ = active_->model_->operation()->state() == OperationState::failed ? TurnFailure::model : TurnFailure::history_commit;
            active_->result_.settle(OperationState::failed);
        }
        active_->settled_ = std::chrono::steady_clock::now();
        active_->record_fact(RunFactKind::settled);
        active_->notifications_.finished = notify(active_->operation()->state() == OperationState::succeeded ? "turn_completed" : "turn_failed",active_->turn());
    }
    active_->completed_call_ = active_->model_->diagnostics();
    active_->context_->seal(); active_->context_.reset(); active_->model_.reset(); models_.collect();
    active_->exited_ = std::chrono::steady_clock::now();
    active_->exit_.settle(OperationState::succeeded); active_->record_fact(RunFactKind::exited); active_.reset();
}
// 清空只允许在实际空闲时执行，不改变回合序号，也不调用模型。
events::ObservationReceipt Session::clear() {
    if (!accepting_ || active_) throw std::logic_error("session: busy or closed");
    history_ = std::make_shared<const MessageHistory>(); return notify("session_cleared",0);
}
// 停止新问答并请求当前工作退出，已经成功提交的历史不回滚。
// 步骤：关闭回合接纳 → 登记当前取消 → 关闭模型入口并推进收尾。
void Session::close() {
    accepting_ = false; if (active_) active_->request_cancel(CancelReason::owner_stop);
    models_.close(); advance();
}
// 释放没有客户端的终态结果，尚未实际退出的回合始终保留。
void Session::collect() noexcept {
    for (auto& record : records_) if (record && record->exit()->state() != OperationState::pending && record.use_count() == 1) record.reset();
}
}
