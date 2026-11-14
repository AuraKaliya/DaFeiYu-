#include <dafeiyu/plugin_dependencies.hpp>
#include <algorithm>
#include <unordered_set>

namespace dfy::plugins {
// 固定目录和引用容量，避免新增依赖导致无界元数据增长。
PluginDependencies::PluginDependencies(std::size_t capacity) : references_(capacity), slots_(capacity) {}
// 从提供方沿现有依赖搜索消费者，自依赖与间接环统一拒绝。
// 步骤：准备有界工作集 → 去重遍历已有边 → 命中消费者即拒绝。
bool PluginDependencies::cyclic(const PluginInstance* consumer, const PluginInstance* provider) const {
    std::vector<const PluginInstance*> pending{provider}; std::unordered_set<const PluginInstance*> seen;
    while (!pending.empty()) {
        const auto* current = pending.back(); pending.pop_back();
        if (current == consumer) return true;
        if (!seen.insert(current).second) continue;
        for (const auto handle : slots_) if (!handle.empty()) {
            const auto& binding = references_.get(handle).binding;
            if (binding->consumer().get() == current) pending.push_back(binding->provider().get());
        }
    }
    return false;
}
// 在所有校验与代码持有成功后发布绑定，不按包名偷换实例。
// 步骤：核对运行实例 → 检查名额及依赖环 → 准备代码和视图 → 签发关系并提交目录。
runtime::Handle PluginDependencies::bind(std::shared_ptr<PluginInstance> consumer, std::shared_ptr<PluginInstance> provider) {
    if (!consumer || !provider) throw std::invalid_argument("dependencies: instances required");
    if (consumer->phase() != InstancePhase::active || provider->phase() != InstancePhase::active)
        throw std::logic_error("dependencies: active instances required");
    const auto slot = std::find(slots_.begin(),slots_.end(),runtime::Handle{});
    if (slot == slots_.end()) throw std::length_error("dependencies: capacity exhausted");
    if (cyclic(consumer.get(),provider.get())) throw std::logic_error("dependencies: cycle");
    auto binding = std::shared_ptr<const DependencyBinding>(new DependencyBinding(std::move(consumer),std::move(provider)));
    const auto handle = references_.insert(std::make_shared<Reference>(Reference{std::move(binding)}),false);
    *slot = handle; return handle;
}
// 新绑定准备失败保留旧路径，新绑定成功也不擅自终止旧使用。
runtime::Handle PluginDependencies::rebind(runtime::Handle old_binding, std::shared_ptr<PluginInstance> provider) {
    const auto old = get(old_binding); return bind(old->consumer(),std::move(provider));
}
// 只归还当前目录的这一份关系，仍存活的视图继续保持提供方代码。
void PluginDependencies::release(runtime::Handle binding) {
    (void)references_.get(binding);
    const auto slot = std::find(slots_.begin(),slots_.end(),binding);
    references_.release(binding); *slot = {};
}
}
