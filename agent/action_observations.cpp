#include <dafeiyu/action_observations.hpp>
#include <set>
#include <stdexcept>

namespace dfy::agent {
// 将已由原集合签发的观察检查组成有界且不可冒充的新绑定。
// 步骤：核对数量与对象唯一性 → 保留原检查 → 签发进程内身份。
ActionObservations::ActionObservations(std::vector<ObservationGuard> guards) : guards_(std::move(guards)) {
    if (guards_.empty() || guards_.size() > 4) throw std::invalid_argument("action observations: expected 1..4 guards");
    std::set<std::pair<runtime::DomainId,std::string>> seen;
    for (const auto& guard : guards_) {
        const auto& value = guard.observation()->metadata();
        if (!seen.emplace(value.source,value.object.id).second) throw std::invalid_argument("action observations: duplicate object");
    }
    binding_ = runtime::next_domain();
}
// 任一来源、对象水位或有效期变化都使原集合不再满足动作前置条件。
bool ActionObservations::current(ObservationTime now) const noexcept {
    if (guards_.empty()) return false;
    for (const auto& guard : guards_) if (!guard.current(now)) return false;
    return true;
}
// 新绑定不能换对象或沿用已消费的采样，旧集合失效不妨碍核对其原身份。
// 步骤：核对新集合当前性及数量 → 按原对象逐项匹配 → 校验更高修订、采样时刻和完整 schema。
bool ActionObservations::succeeds(const ActionObservations& previous, ObservationTime now, ObservationTime not_before) const noexcept {
    if (binding_ == previous.binding_ || guards_.size() != previous.guards_.size() || !current(now)) return false;
    for (const auto& guard : guards_) {
        bool matched = false; const auto value = guard.observation();
        for (const auto& old : previous.guards_) if (guard.same_object(old)) {
            const auto original = old.observation();
            if (value->metadata().revision <= original->metadata().revision || value->metadata().sampled < not_before ||
                !value->payload().descriptor()->equivalent(*original->payload().descriptor())) return false;
            matched = true; break;
        }
        if (!matched) return false;
    }
    return true;
}
// 模型读取的观察与动作复核始终指向相同原值，查询不刷新资格。
std::vector<std::shared_ptr<const ObservationRecord>> ActionObservations::observations() const {
    std::vector<std::shared_ptr<const ObservationRecord>> result; result.reserve(guards_.size());
    for (const auto& guard : guards_) result.push_back(guard.observation());
    return result;
}
}
