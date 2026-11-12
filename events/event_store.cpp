#include <dafeiyu/event_store.hpp>
#include "sequence.hpp"

namespace dfy::events {
// 分别建立事件接纳身份及引用签发域，不将句柄编号当作事件序号。
EventStore::EventStore(std::size_t reference_capacity, std::size_t max_payload_bytes)
    : domain_(runtime::next_domain()), references_(reference_capacity), max_payload_bytes_(max_payload_bytes) {}

// 顺序接纳宿主不可变载荷，拒绝不产生对外事件或消耗接纳序号。
// 步骤：核对载荷与正文上限 → 检查序号余量 → 创建事件并签发引用 → 推进接纳序号。
runtime::Handle EventStore::accept(std::shared_ptr<const runtime::StructValue> payload) {
    if (!payload) throw std::invalid_argument("event: payload required");
    if (payload->payload_bytes() > max_payload_bytes_) throw std::length_error("event: payload limit exceeded");
    if (!next_sequence_) throw std::overflow_error("event: sequence exhausted");
    auto event = std::make_shared<const EventRecord>(EventRecord{{domain_, next_sequence_}, std::move(payload)});
    const auto owned = references_.insert(std::move(event));
    next_sequence_ = detail::following_sequence(next_sequence_);
    return owned;
}
}
