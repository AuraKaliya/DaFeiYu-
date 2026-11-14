#include <dafeiyu/serial_domain.hpp>
#include "id_sequence.hpp"

namespace dfy::runtime {
// 建立固定记录槽，失败不发布半个顺序域。
SerialDomain::SerialDomain(std::size_t capacity) : domain_(next_domain()), requests_(capacity) {
    if (!capacity) throw std::invalid_argument("serial: positive capacity required");
}
// 取消未授予等待，已经授予的记录保留真实阶段，不宣称业务已经结束。
SerialDomain::~SerialDomain() { close(SerialClose::cancel); }
// 在全部分配成功后签发并登记等待，容量或编号耗尽不留下部分记录。
// 步骤：回收终态并核对接纳 → 完整创建请求 → 签发非回绕序号 → 提交记录。
std::shared_ptr<SerialRequest> SerialDomain::submit() {
    collect();
    if (mode_) throw std::logic_error("serial: admission closed");
    for (auto& slot : requests_) {
        if (slot) continue;
        auto request = std::shared_ptr<SerialRequest>(new SerialRequest(domain_));
        request->sequence_ = detail::allocate_id(last_sequence_);
        slot = request; ++size_;
        return request;
    }
    throw std::length_error("serial: request capacity exhausted");
}
// 保留活动占用并按接受顺序授予，异步等待期间调用 advance 不会让出当前域。
// 步骤：回收终态 → 结算等待取消 → 保留活动占用或寻找最早等待 → 发布授予结果。
void SerialDomain::advance() {
    collect();
    for (auto& request : requests_) {
        if (!request || request->phase_ != SerialPhase::waiting || request->admission()->cancellation_reason() == CancelReason::none) continue;
        request->phase_ = SerialPhase::cancelled;
        request->source_.settle(OperationState::cancelled);
    }
    if (active_) return;
    std::optional<std::size_t> oldest;
    for (std::size_t index = 0; index < requests_.size(); ++index) {
        const auto& request = requests_[index];
        if (!request || request->phase_ != SerialPhase::waiting) continue;
        if (!oldest || request->sequence_ < requests_[*oldest]->sequence_) oldest = index;
    }
    if (!oldest) return;

    auto& request = *requests_[*oldest];
    request.phase_ = SerialPhase::granted; active_ = oldest;
    request.source_.settle(OperationState::succeeded);
}
// 只归还本域当前授予的占用，等待、旧终态及其他域均不能完成当前处理。
// 步骤：核对域身份及当前记录 → 标记占用释放 → 清空活动槽。
void SerialDomain::finish(const std::shared_ptr<const SerialRequest>& request) {
    if (!request || request->domain() != domain_) throw std::invalid_argument("serial: foreign request");
    if (!active_ || requests_[*active_].get() != request.get()) throw std::logic_error("serial: request not active");

    requests_[*active_]->phase_ = SerialPhase::released;
    active_.reset();
}
// 停止接纳后排空或取消等待，取消不会撤销已经授予的执行资格。
// 步骤：校验方式并保留关闭边界 → 固定或升级关闭方式 → 按需取消等待并推进结算。
void SerialDomain::close(SerialClose mode) {
    if (mode != SerialClose::drain && mode != SerialClose::cancel) throw std::invalid_argument("serial: invalid close mode");
    if (closed()) return;
    if (!mode_ || mode == SerialClose::cancel) mode_ = mode;
    if (*mode_ != SerialClose::cancel) return;

    for (auto& request : requests_)
        if (request && request->phase_ == SerialPhase::waiting) request->request_cancel(CancelReason::owner_stop);
    advance();
}
// 统计实际等待阶段，不将已请求取消但尚未结算的记录提前排除。
std::size_t SerialDomain::pending() const noexcept {
    std::size_t count = 0;
    for (const auto& request : requests_) if (request && request->phase_ == SerialPhase::waiting) ++count;
    return count;
}
// 仅释放没有客户端的终态记录，活动和等待责任始终由域保持。
void SerialDomain::collect() noexcept {
    for (auto& request : requests_) {
        if (!request || (request->phase_ != SerialPhase::released && request->phase_ != SerialPhase::cancelled) || request.use_count() != 1) continue;
        request.reset(); --size_;
    }
}
}
