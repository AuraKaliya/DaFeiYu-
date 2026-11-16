#include <dafeiyu/observation.hpp>
#include <dafeiyu/utf8.hpp>
#include <stdexcept>

namespace dfy::agent {
struct ObservationBinding {
    runtime::DomainId id, instance;
    ObservationTrust trust;
    bool active = true;
};
struct ObservationSnapshot::State {
    std::shared_ptr<const ObservationBinding> binding;
    ObservationMetadata metadata;
    std::shared_ptr<const runtime::StructValue> payload;
};
// 在签发身份之前检查来源元数据，观察正文无权选择提供实例或信任等级。
ObservationSource::ObservationSource(runtime::DomainId provider_instance, ObservationTrust trust) {
    if (!provider_instance || (trust != ObservationTrust::external_data && trust != ObservationTrust::host_verified))
        throw std::invalid_argument("observation: invalid provider or trust");
    binding_ = std::make_shared<ObservationBinding>(runtime::next_domain(),provider_instance,trust);
}
// 来源销毁后旧值仍可回顾，但不能被当成当前可用观察。
ObservationSource::~ObservationSource() { revoke(); }
// 返回唯一来源域，不复用提供实例编号作为来源身份。
runtime::DomainId ObservationSource::id() const noexcept { return binding_->id; }
// 创建有界来源快照，过期、未来及失效资格由读取方在明确时刻判断。
// 步骤：校验来源、对象及载荷 → 核对有限时间区间 → 固定元数据并共享不可变值。
ObservationSnapshot ObservationSource::capture(ObservationObject object, std::uint64_t revision, ObservationTime sampled,
    ObservationTime expires, std::shared_ptr<const runtime::StructValue> payload) const {
    if (!binding_->active || !object.generation || !revision || object.id.empty() || object.id.size() > 128 ||
        !runtime::valid_utf8(object.id) || !payload || payload->payload_bytes() > 64*1024 || payload->node_count() > 1024)
        throw std::invalid_argument("observation: invalid source, object or payload");
    for (const unsigned char byte : object.id) if (byte < 32 || byte == 127) throw std::invalid_argument("observation: invalid object ID");
    const auto ttl = std::chrono::duration_cast<ObservationTime::duration>(std::chrono::hours(1));
    const auto maximum = sampled > ObservationTime::max()-ttl ? ObservationTime::max() : sampled+ttl;
    if (expires <= sampled || expires > maximum) throw std::invalid_argument("observation: invalid validity interval");
    ObservationMetadata metadata{binding_->id,binding_->instance,binding_->trust,std::move(object),revision,sampled,expires};
    return ObservationSnapshot(std::make_shared<const ObservationSnapshot::State>(binding_,std::move(metadata),std::move(payload)));
}
// 撤销只改变来源资格，不修改快照中的对象、修订或原载荷。
void ObservationSource::revoke() noexcept { binding_->active = false; }
// 借用原不可变元数据，不重新解释载荷内自报的来源或修订。
const ObservationMetadata& ObservationSnapshot::metadata() const noexcept { return state_->metadata; }
// 返回数据引用，不延长来源有效性或持有插件代码。
std::shared_ptr<const runtime::StructValue> ObservationSnapshot::payload() const noexcept { return state_->payload; }
// 以来源撤销优先裁定时间资格，不推断对象是否已经出现更高修订。
ObservationValidity ObservationSnapshot::validity(ObservationTime now) const noexcept {
    if (!state_->binding->active) return ObservationValidity::source_revoked;
    if (now < state_->metadata.sampled) return ObservationValidity::future;
    return now >= state_->metadata.expires ? ObservationValidity::expired : ObservationValidity::fresh;
}
}
