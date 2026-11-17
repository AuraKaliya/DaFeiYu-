#include <dafeiyu/observation_inbox.hpp>
#include <algorithm>
#include <set>
#include <stdexcept>

namespace dfy::agent {
struct ObservationWatermark { std::uint64_t generation = 0, revision = 0; };
// 固定订阅范围及有限记录空间，不在对象满时默默淘汰旧对象的修订水位。
// 步骤：核对来源和容量 → 预留对象槽 → 建立保留版本的分层预算。
ObservationInbox::ObservationInbox(std::vector<runtime::DomainId> sources, std::size_t objects,
    runtime::BudgetAmount retained, std::shared_ptr<runtime::Budget> parent) : sources_(std::move(sources)) {
    if (sources_.size() > 64 || !objects || objects > 64 || retained.items <= objects || retained.items > 256 || retained.bytes > 16*1024*1024)
        throw std::invalid_argument("observations: invalid scope or limits");
    std::set<runtime::DomainId> unique;
    for (const auto source : sources_) if (!source || !unique.insert(source).second) throw std::invalid_argument("observations: invalid source grant");
    records_.resize(objects); budget_ = std::make_shared<runtime::Budget>(retained,parent); id_ = runtime::next_domain();
}
// 只撤销当前性检查，不丢弃客户端仍持有的历史观察值。
ObservationInbox::~ObservationInbox() { *alive_ = false; }
// 普通采样永远不宣称补齐中间遗漏，断档保持到明确的完整重采样。
ObservationReceipt ObservationInbox::accept(const ObservationSnapshot& value, ObservationTime now) { return accept_impl(value,now,false); }
// 只合并状态采样；旧代次、旧修订、冲突或容量不足均不改变当前值。
// 步骤：检查来源及时效 → 定位对象及版本水位 → 判断重复或断档 → 预留完整预算 → 原子替换记录。
ObservationReceipt ObservationInbox::accept_impl(const ObservationSnapshot& value, ObservationTime now, bool baseline) {
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
    const bool replacing = slot != records_.end(); bool gap = !baseline && incoming.revision != 1;
    if (replacing) {
        const auto& record = **slot; const auto& current = record.metadata();
        if (incoming.object.generation < current.object.generation) return {A::stale_generation};
        if (incoming.object.generation == current.object.generation) {
            if (incoming.revision < current.revision) return {A::stale_revision};
            if (incoming.revision == current.revision) {
                const bool same = incoming.sampled == current.sampled && incoming.expires == current.expires && value.payload() == record.value_.payload();
                return {same ? A::duplicate : A::conflict,record.gap_};
            }
            gap = !baseline && (record.gap_ || incoming.revision-current.revision > 1);
        }
    } else {
        slot = std::find(records_.begin(),records_.end(),nullptr);
        if (slot == records_.end()) return {A::capacity};
    }
    try {
        auto charge = budget_->acquire({1,value.payload()->payload_bytes()+incoming.object.id.size()});
        auto watermark = replacing ? (*slot)->watermark_ : std::make_shared<ObservationWatermark>();
        auto accepted = std::shared_ptr<const ObservationRecord>(new ObservationRecord(std::move(charge),value,gap,watermark));
        *watermark = {incoming.object.generation,incoming.revision};
        *slot = std::move(accepted);
    } catch (const std::length_error&) { return {A::capacity}; }
    return {replacing ? A::replaced : A::inserted,gap};
}
// 固定发起重采样时看到的原对象和修订，未知对象没有可覆盖的基线。
std::optional<ObservationRefresh> ObservationInbox::refresh_ticket(runtime::DomainId source, std::string_view object) const {
    const auto current = latest(source,object); if (!current) return {};
    const auto& data = current->metadata(); return ObservationRefresh{id_,source,data.object,data.revision};
}
// 完整重采样不能覆盖期间出现的新状态，也不因失败提前清除断档。
// 步骤：核对原集合及对象水位 → 检查同代次的新采样 → 复用接纳并原子建立完整基线。
ObservationReceipt ObservationInbox::refresh(const ObservationRefresh& expected, const ObservationSnapshot& value, ObservationTime now) {
    const auto current = latest(expected.source,expected.object.id); const auto& incoming = value.metadata();
    if (expected.inbox != id_ || !current || current->metadata().object.generation != expected.object.generation ||
        current->metadata().revision != expected.revision || incoming.source != expected.source || incoming.object.id != expected.object.id ||
        incoming.object.generation != expected.object.generation || incoming.revision <= expected.revision)
        return {ObservationAdmission::stale_refresh};
    return accept_impl(value,now,true);
}
// 返回只读保留句柄，不刷新时效、推进来源或移除过期水位。
std::shared_ptr<const ObservationRecord> ObservationInbox::latest(runtime::DomainId source, std::string_view object) const noexcept {
    for (const auto& record : records_) if (record && record->metadata().source == source && record->metadata().object.id == object) return record;
    return {};
}
// 只对同一集合的完整当前值签发检查，不能从旧快照恢复行动资格。
// 步骤：核对原记录和当前对象 → 检查来源、时间及断档 → 保留值与弱寿命引用。
ObservationGuard ObservationInbox::guard(std::shared_ptr<const ObservationRecord> observation, ObservationTime now) const {
    if (!observation || latest(observation->metadata().source,observation->metadata().object.id) != observation ||
        observation->revision_gap() || observation->validity(now) != ObservationValidity::fresh)
        throw std::invalid_argument("observations: current complete record required");
    return {std::move(observation),alive_};
}
// 原值必须仍对应本对象的最新水位，集合或来源失效不能靠旧值持有恢复。
bool ObservationGuard::current(ObservationTime now) const noexcept {
    const auto alive = alive_.lock(); const auto& metadata = observation_->metadata();
    return alive && *alive && !observation_->revision_gap() && observation_->validity(now) == ObservationValidity::fresh &&
        observation_->watermark_->generation == metadata.object.generation && observation_->watermark_->revision == metadata.revision;
}
}
