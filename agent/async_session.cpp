#include <dafeiyu/async_session.hpp>
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
// 完整准备会话数据和有界模型客户端，不创建待执行问答。
AsyncSession::AsyncSession(model::Config config, std::shared_ptr<events::BroadcastChannel> events,
    std::size_t result_capacity, model::Completion completion)
    : config_(std::move(config)), events_(std::move(events)), descriptor_(event_descriptor()), id_(next_domain()),
      records_(result_capacity), models_(1,std::move(completion)) {
    if (!events_ || !result_capacity) throw std::invalid_argument("session: event channel and positive result capacity required");
}
// 兜底关闭时先保留所有回合责任，实际模型退出后再结算回合退出。
AsyncSession::~AsyncSession() { close(); models_.wait_closed(); advance(); }
// 以不可变事件传递会话事实，观察接纳或分配失败只形成独立回执。
// 步骤：构造类型化字段 → 创建宿主载荷 → 接纳观察通知或返回失败回执。
events::ObservationReceipt AsyncSession::notify(std::string_view kind, std::uint64_t turn) noexcept {
    try {
        const NamedValue values[] = {{"kind",Value::from_string(kind,32)},{"session",Value::from_uint64(id_)},
            {"turn",Value::from_uint64(turn)},{"messages",Value::from_uint64(history_.size())}};
        return events::observe(*events_,std::make_shared<const StructValue>(StructValue::create(descriptor_,values,64)));
    } catch (...) { return {events::ObservationStatus::failed,{}}; }
}
// 完整准备成功后才接受回合，拒绝不消耗回合号或改变已提交上下文。
// 步骤：检查空闲、输入和结果槽 → 准备待提交历史及回合 → 提交后台模型 → 固定接纳并通知开始。
std::shared_ptr<TurnRequest> AsyncSession::begin(const std::string& prompt) {
    collect();
    if (!accepting_ || active_) throw std::logic_error("session: busy or closed");
    if (prompt.empty() || prompt.find_first_not_of(" \t\r\n") == std::string::npos || prompt.size() > 1024*1024)
        throw std::invalid_argument("session: nonblank prompt within 1 MiB required");
    if (turn_ == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("session: turn exhausted");
    for (auto& slot : records_) if (!slot) {
        auto pending = history_; pending.push_back({model::Role::user,prompt});
        auto record = std::shared_ptr<TurnRequest>(new TurnRequest(turn_+1));
        auto request = models_.submit(config_,pending);
        pending_.swap(pending); model_request_ = std::move(request); active_ = record; slot = record; ++turn_;
        record->notifications_.started = notify("turn_started",turn_); return record;
    }
    throw std::length_error("session: retained turn capacity exhausted");
}
// 取消优先于同次推进观察到的模型结果，完整问答准备成功才原子提交历史。
// 步骤：裁定回合取消 → 推进后台结果 → 准备并提交成功或保留失败历史 → 通知唯一终态 → 确认实际退出。
void AsyncSession::advance() {
    if (active_ && active_->operation()->state() == OperationState::pending && active_->operation()->cancellation_reason() != CancelReason::none) {
        model_request_->request_cancel(active_->operation()->cancellation_reason()); active_->result_.settle(OperationState::cancelled);
        pending_.clear(); active_->notifications_.finished = notify("turn_cancelled",active_->turn());
    }
    models_.advance();
    if (!active_ || model_request_->exit()->state() == OperationState::pending) return;
    if (active_->operation()->state() == OperationState::pending) {
        try {
            auto answer = model_request_->answer(); std::size_t bytes = answer.size();
            for (const auto& message : pending_) bytes += message.content.size();
            if (bytes > 1024*1024) throw std::length_error("session: history exceeds 1 MiB; clear context before retrying");
            pending_.push_back({model::Role::assistant,answer});
            history_.swap(pending_); active_->answer_ = std::move(answer); active_->result_.settle(OperationState::succeeded);
        } catch (...) { active_->error_ = std::current_exception(); active_->result_.settle(OperationState::failed); }
        active_->notifications_.finished = notify(active_->operation()->state() == OperationState::succeeded ? "turn_completed" : "turn_failed",active_->turn());
    }
    pending_.clear(); model_request_.reset(); models_.collect();
    active_->exit_.settle(OperationState::succeeded); active_.reset();
}
// 清空只允许在实际空闲时执行，不改变回合序号，也不调用模型。
events::ObservationReceipt AsyncSession::clear() {
    if (!accepting_ || active_) throw std::logic_error("session: busy or closed");
    history_.clear(); return notify("session_cleared",0);
}
// 停止新问答并请求当前工作退出，已经成功提交的历史不回滚。
void AsyncSession::close() {
    accepting_ = false; if (active_) active_->request_cancel(CancelReason::owner_stop);
    models_.close(); advance();
}
// 释放没有客户端的终态结果，尚未实际退出的回合始终保留。
void AsyncSession::collect() noexcept {
    for (auto& record : records_) if (record && record->exit()->state() != OperationState::pending && record.use_count() == 1) record.reset();
}
}
