#include <dafeiyu/observation_inbox.hpp>
#include <algorithm>
#include <set>
#include <stdexcept>

namespace dfy::agent {
// 固定订阅范围及有限记录空间，不在对象满时默默淘汰旧对象的修订水位。
// 步骤：核对来源和容量 → 预留对象槽 → 建立保留版本的分层预算。
ObservationInbox::ObservationInbox(std::vector<runtime::DomainId> sources, std::size_t objects,
    runtime::BudgetAmount retained, std::shared_ptr<runtime::Budget> parent) : sources_(std::move(sources)) {
    if (sources_.size() > 64 || !objects || objects > 64 || retained.items <= objects || retained.items > 256 || retained.bytes > 16*1024*1024)
        throw std::invalid_argument("observations: invalid scope or limits");
    std::set<runtime::DomainId> unique;
    for (const auto source : sources_) if (!source || !unique.insert(source).second) throw std::invalid_argument("observations: invalid source grant");
    records_.resize(objects); budget_ = std::make_shared<runtime::Budget>(retained,parent);
}
// 只合并状态采样；旧代次、旧修订、冲突或容量不足均不改变当前值。
// 步骤：检查来源及时效 → 定位对象及版本水位 → 判断重复或断档 → 预留完整预算 → 原子替换记录。
ObservationReceipt ObservationInbox::accept(const ObservationSnapshot& value, ObservationTime now) {
    using A = ObservationAdmission; const auto& incoming = value.metadata();
    if (std::find(sources_.begin(),sources_.end(),incoming.source) == sources_.end()) return {A::not_allowed};
    switch (value.validity(now)) {
    case ObservationValidity::future: return {A::future};
    case ObservationValidity::expired: return {A::expired};
    case ObservationValidity::source_revoked: return {A::source_revoked};
    case ObservationValidity::fresh: break;
    }
    auto slot = std::find_if(records_.begin(),records_.end(),[&](const auto& record) {
        return record && record->metadata().source == incoming.source && record->metadata().object.id == incoming.object.id;
    });
    const bool replacing = slot != records_.end(); bool gap = incoming.revision != 1;
    if (replacing) {
        const auto& record = **slot; const auto& current = record.metadata();
        if (incoming.object.generation < current.object.generation) return {A::stale_generation};
        if (incoming.object.generation == current.object.generation) {
            if (incoming.revision < current.revision) return {A::stale_revision};
            if (incoming.revision == current.revision) {
                const bool same = incoming.sampled == current.sampled && incoming.expires == current.expires && value.payload() == record.value_.payload();
                return {same ? A::duplicate : A::conflict,record.gap_};
            }
            gap = record.gap_ || incoming.revision-current.revision > 1;
        }
    } else {
        slot = std::find(records_.begin(),records_.end(),nullptr);
        if (slot == records_.end()) return {A::capacity};
    }
    try {
        auto charge = budget_->acquire({1,value.payload()->payload_bytes()+incoming.object.id.size()});
        auto accepted = std::shared_ptr<const ObservationRecord>(new ObservationRecord(std::move(charge),value,gap));
        *slot = std::move(accepted);
    } catch (const std::length_error&) { return {A::capacity}; }
    return {replacing ? A::replaced : A::inserted,gap};
}
// 返回只读保留句柄，不刷新时效、推进来源或移除过期水位。
std::shared_ptr<const ObservationRecord> ObservationInbox::latest(runtime::DomainId source, std::string_view object) const noexcept {
    for (const auto& record : records_) if (record && record->metadata().source == source && record->metadata().object.id == object) return record;
    return {};
}
}
