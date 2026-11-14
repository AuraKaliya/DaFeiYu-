#include <dafeiyu/serial_wait_graph.hpp>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace dfy::runtime {
// 只用当前有效依赖检查可达性，旧请求结束后不阻挡同域的新请求。
// 步骤：拒绝同域自等待 → 建立有效邻接关系 → 从目标搜索来源 → 返回是否成环。
bool SerialWaitGraph::creates_cycle(DomainId from, DomainId to) const {
    if (from == to) return true;
    std::unordered_map<DomainId,std::vector<DomainId>> outgoing;
    for (const auto handle : slots_) {
        if (handle.empty()) continue;
        const auto& edge = references_.get(handle);
        if (edge.active()) outgoing[edge.from].push_back(edge.to);
    }

    std::vector<DomainId> pending{to}; std::unordered_set<DomainId> visited;
    while (!pending.empty()) {
        const auto domain = pending.back(); pending.pop_back();
        if (domain == from) return true;
        if (!visited.insert(domain).second) continue;
        const auto found = outgoing.find(domain);
        if (found != outgoing.end()) pending.insert(pending.end(),found->second.begin(),found->second.end());
    }
    return false;
}
// 登记已授予处理的显式等待，多个相同依赖各自计费并独立归还。
// 步骤：核对来源与目标阶段 → 检查容量及等待环 → 创建独立登记 → 提交槽位。
Handle SerialWaitGraph::add(std::shared_ptr<const SerialRequest> owner, std::shared_ptr<const SerialRequest> target) {
    if (!owner || !target) throw std::invalid_argument("serial wait: requests required");
    if (owner->phase() != SerialPhase::granted && owner->phase() != SerialPhase::running)
        throw std::logic_error("serial wait: owner must hold domain");
    if (target->phase() == SerialPhase::released || target->phase() == SerialPhase::cancelled) return {};
    const auto slot = std::find(slots_.begin(),slots_.end(),Handle{});
    if (slot == slots_.end()) throw ReferenceError(ReferenceFailure::exhausted);
    if (creates_cycle(owner->domain(),target->domain())) throw std::logic_error("serial wait: dependency cycle");

    const auto handle = references_.insert(std::make_shared<const Dependency>(Dependency{owner,target,owner->domain(),target->domain()}),false);
    *slot = handle;
    return handle;
}
// 只归还对应登记，不改变请求阶段、实际处理或顺序域占用。
// 步骤：核对登记身份 → 定位对应槽位 → 归还引用并清空槽位。
void SerialWaitGraph::release(Handle dependency) {
    if (dependency.empty()) return;
    (void)references_.get(dependency);
    const auto slot = std::find(slots_.begin(),slots_.end(),dependency);

    references_.release(dependency);
    *slot = {};
}
// 返回依赖是否仍有效，空登记表示无需等待，其他错误身份仍明确拒绝。
bool SerialWaitGraph::active(Handle dependency) const {
    return !dependency.empty() && references_.get(dependency).active();
}
}
