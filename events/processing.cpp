#include <dafeiyu/processing.hpp>

namespace dfy::events {
using namespace runtime;
// 完整创建后绑定顺序，任一准备失败均不消耗调用方事件或改变顺序请求。
// 步骤：准备独立事件与结果 → 校验顺序参数成对提供 → 独占绑定请求。
ProcessingSource::ProcessingSource(const EventLease& event, std::shared_ptr<SerialDomain> domain,
    std::shared_ptr<const SerialRequest> ordering)
    : domain_(std::move(domain)), record_(new ProcessingRecord(event,std::move(ordering))) {
    if (static_cast<bool>(domain_) != static_cast<bool>(record_->ordering_))
        throw std::invalid_argument("processing: domain and request must be supplied together");
    if (domain_) domain_->bind(record_->ordering_);
}
// 仅收回尚未开始的处理，运行中的完成和退出必须由仍存活的宿主控制器负责。
// 步骤：保留已开始或已退出的事实 → 请求未开始项停止 → 撤回顺序并确认无需运行退出。
ProcessingSource::~ProcessingSource() {
    if (record_->phase_ != ProcessingPhase::waiting) return;
    record_->request_cancel(CancelReason::owner_stop);
    cancel_unstarted();
}
// 固定开始前取消，已授予资格可归还，但不改写授予事实或驱动后续处理。
// 步骤：按顺序阶段撤回占用 → 结算业务取消 → 确认实际退出。
void ProcessingSource::cancel_unstarted() {
    if (domain_ && record_->ordering_->phase() != SerialPhase::cancelled)
        domain_->withdraw(record_->ordering_,record_->exit()->cancellation_reason());
    record_->completion_.settle(OperationState::cancelled);
    record_->phase_ = ProcessingPhase::exited;
    record_->exit_.settle(OperationState::succeeded);
}
// 开始只依赖已取得的资格，取消和域关闭在实际运行前优先裁定。
// 步骤：拒绝重复开始 → 合并顺序取消 → 撤回取消处理或等待资格 → 确认实际开始。
StartResult ProcessingSource::start() {
    if (record_->phase_ == ProcessingPhase::exited) return StartResult::exited;
    if (record_->phase_ != ProcessingPhase::waiting) throw std::logic_error("processing: already started");
    if (domain_ && record_->ordering_->phase() == SerialPhase::cancelled)
        record_->request_cancel(record_->ordering_->admission()->cancellation_reason());
    if (record_->exit()->cancellation_reason() != CancelReason::none) { cancel_unstarted(); return StartResult::exited; }
    if (domain_ && record_->ordering_->phase() == SerialPhase::waiting) return StartResult::waiting;

    if (domain_) domain_->begin(record_->ordering_);
    record_->phase_ = ProcessingPhase::running;
    return StartResult::started;
}
// 固定唯一业务结果，不把取消意图当作已完成，也不在此归还顺序域。
void ProcessingSource::complete(OperationState terminal) {
    if (record_->phase_ != ProcessingPhase::running) throw std::logic_error("processing: running work required");
    record_->completion_.settle(terminal);
}
// 在实际处理退出后发布退出确认，业务结果和仍被保留的数据均不改写。
// 步骤：核对运行及业务终态 → 归还顺序占用 → 固定退出阶段及确认。
void ProcessingSource::finish() {
    if (record_->phase_ != ProcessingPhase::running || record_->completion()->state() == OperationState::pending)
        throw std::logic_error("processing: completed running work required");
    if (domain_) domain_->finish(record_->ordering_);
    record_->phase_ = ProcessingPhase::exited;
    record_->exit_.settle(OperationState::succeeded);
}
}
