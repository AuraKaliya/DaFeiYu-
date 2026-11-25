#include <dafeiyu/event_store.hpp>
#include "sequence.hpp"
#include <limits>

namespace dfy::events {
// 分别建立事件接纳身份、引用域和存储预算，默认字节上限按引用数乘单事件上限饱和计算。
EventStore::EventStore(std::size_t reference_capacity, std::size_t max_payload_bytes, std::shared_ptr<runtime::Budget> parent_budget)
    : domain_(runtime::next_domain()), references_(reference_capacity), max_payload_bytes_(max_payload_bytes),
      budget_(std::make_shared<runtime::Budget>(runtime::BudgetAmount{reference_capacity,
        max_payload_bytes > std::numeric_limits<std::size_t>::max() / reference_capacity ? std::numeric_limits<std::size_t>::max() :
        max_payload_bytes * reference_capacity},parent_budget)) {}

// 接纳单份持有，复用批量接纳的原子签发及序号规则。
runtime::Handle EventStore::accept(std::shared_ptr<const runtime::StructValue> payload) {
    return accept_batch(std::move(payload), 1).front();
}

// 为同一次事件接纳准备全部接收方引用，失败不留下半批或推进序号。
// 步骤：校验载荷及引用预算 → 准备输出并申请一次存储计费 → 签发引用或回滚 → 推进唯一接纳序号。
std::vector<runtime::Handle> EventStore::accept_batch(std::shared_ptr<const runtime::StructValue> payload, std::size_t copies) {
    if (!payload || !copies) throw std::invalid_argument("event: payload and positive reference count required");
    if (payload->payload_bytes() > max_payload_bytes_) throw std::length_error("event: payload limit exceeded");
    if (!next_sequence_) throw std::overflow_error("event: sequence exhausted");
    if (copies > references_.available()) throw runtime::ReferenceError(runtime::ReferenceFailure::exhausted);
    std::vector<runtime::Handle> owned;
    owned.reserve(copies);
    auto charge = budget_->acquire({1,payload->payload_bytes()});
    auto event = std::make_shared<const EventRecord>(EventRecord{std::move(charge),{domain_, next_sequence_},std::move(payload)});
    try {
        for (std::size_t index = 0; index < copies; ++index) owned.push_back(references_.insert(event));
    } catch (...) {
        for (const auto reference : owned) references_.release(reference);
        throw;
    }
    next_sequence_ = detail::following_sequence(next_sequence_);
    return owned;
}
}
