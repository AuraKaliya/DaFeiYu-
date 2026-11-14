#include <dafeiyu/processing_queue.hpp>

namespace dfy::events {
using namespace runtime;
// 预分配全部处理槽和可选顺序域，不在过载时扩容。
ProcessingQueue::ProcessingQueue(std::size_t capacity, bool ordered)
    : sources_(capacity), domain_(ordered ? std::make_shared<SerialDomain>(capacity) : nullptr) {
    if (!capacity) throw std::invalid_argument("processing queue: positive capacity required");
}
// 收回未开始责任；运行中处理的宿主必须先完成并确认退出。
ProcessingQueue::~ProcessingQueue() { close(CloseMode::cancel); }
// 在处理接纳点固定顺序，所有可能失败的数据准备均先于顺序签发。
// 步骤：回收并核对接纳预算 → 准备数据和完成源 → 签发并绑定顺序 → 提交固定槽。
std::shared_ptr<ProcessingRecord> ProcessingQueue::submit(const EventLease& event) {
    collect();
    if (mode_) throw std::logic_error("processing queue: admission closed");
    for (auto& slot : sources_) {
        if (slot) continue;
        auto source = std::make_unique<ProcessingSource>(event);
        if (domain_) {
            const auto order = domain_->submit();
            domain_->bind(order);
            source->domain_ = domain_; source->record_->ordering_ = order;
        }
        const auto record = source->record(); slot = std::move(source); ++size_;
        return record;
    }
    throw std::length_error("processing queue: capacity exhausted");
}
// 找到本队列仍持有的完整处理源，错误身份不触碰其他处理。
ProcessingSource& ProcessingQueue::owned(const std::shared_ptr<const ProcessingRecord>& record) const {
    if (record) for (const auto& source : sources_) if (source && source->record_ == record) return *source;
    throw std::invalid_argument("processing queue: unknown record");
}
// 只授予执行资格，业务函数由宿主在独立调用边界执行。
// 步骤：回收终态 → 结算开始前取消 → 推进顺序授予。
void ProcessingQueue::advance() {
    collect();
    for (const auto& source : sources_) if (source && source->record_->phase() == ProcessingPhase::waiting &&
        source->record_->exit()->cancellation_reason() != CancelReason::none) (void)source->start();
    if (domain_) domain_->advance();
}
// 找到尚未开始且具有执行资格的记录，默认顺序域每次仅允许一项。
std::shared_ptr<ProcessingRecord> ProcessingQueue::ready() const noexcept {
    for (const auto& source : sources_) if (source && source->record_->phase() == ProcessingPhase::waiting &&
        (!domain_ || source->record_->ordering()->phase() == SerialPhase::granted)) return source->record();
    return {};
}
// 在相同宿主线程裁定指定记录的实际开始。
StartResult ProcessingQueue::start(const std::shared_ptr<const ProcessingRecord>& record) { return owned(record).start(); }
// 仅转交指定记录的业务完成权，不使后续项获得执行资格。
void ProcessingQueue::complete(const std::shared_ptr<const ProcessingRecord>& record, OperationState terminal) { owned(record).complete(terminal); }
// 确认指定处理实际退出，后续项仍需独立推进才能取得资格。
void ProcessingQueue::finish(const std::shared_ptr<const ProcessingRecord>& record) { owned(record).finish(); }
// 取消开始前处理并请求运行项退出，排空只停止新接纳。
// 步骤：核对关闭方式 → 固定或升级关闭 → 登记取消并撤回未开始项 → 关闭顺序域。
void ProcessingQueue::close(CloseMode mode) {
    if (mode != CloseMode::drain && mode != CloseMode::cancel) throw std::invalid_argument("processing queue: invalid close mode");
    if (closed()) return;
    if (!mode_ || mode == CloseMode::cancel) mode_ = mode;
    if (*mode_ == CloseMode::cancel) {
        for (const auto& source : sources_) if (source) {
            source->record_->request_cancel(CancelReason::owner_stop);
            if (source->record_->phase() == ProcessingPhase::waiting) (void)source->start();
        }
    }
    if (domain_) domain_->close(*mode_ == CloseMode::drain ? SerialClose::drain : SerialClose::cancel);
}
// 全部记录实际退出后逻辑关闭，终态快照不再阻挡关闭但继续占槽。
bool ProcessingQueue::closed() const noexcept {
    if (!mode_) return false;
    for (const auto& source : sources_) if (source && source->record_->phase() != ProcessingPhase::exited) return false;
    return true;
}
// 归还无外部持有的退出记录，再回收其顺序记录名额。
void ProcessingQueue::collect() noexcept {
    for (auto& source : sources_) if (source && source->record_->phase() == ProcessingPhase::exited && source->record_.use_count() == 1) {
        source.reset(); --size_;
    }
    if (domain_) domain_->collect();
}
}
