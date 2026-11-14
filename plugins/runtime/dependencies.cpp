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
// 先固定依赖闭包，再按无剩余使用者的叶子取出，所有计划分配发生在停止前。
// 步骤：收集递归依赖方 → 反复选择无在途依赖方的实例 → 返回消费者先于提供方的计划。
std::vector<std::shared_ptr<PluginInstance>> PluginDependencies::stop_plan(const std::shared_ptr<PluginInstance>& provider) const {
    std::vector<std::shared_ptr<PluginInstance>> pending{provider}, plan;
    for (std::size_t index = 0; index < pending.size(); ++index) {
        for (const auto handle : slots_) if (!handle.empty()) {
            const auto& binding = references_.get(handle).binding;
            if (binding->provider() == pending[index] && std::find(pending.begin(),pending.end(),binding->consumer()) == pending.end())
                pending.push_back(binding->consumer());
        }
    }
    plan.reserve(pending.size());
    while (!pending.empty()) {
        const auto leaf = std::find_if(pending.begin(),pending.end(),[&](const auto& candidate) {
            for (const auto handle : slots_) if (!handle.empty()) {
                const auto& binding = references_.get(handle).binding;
                if (binding->provider() == candidate && std::find(pending.begin(),pending.end(),binding->consumer()) != pending.end()) return false;
            }
            return true;
        });
        if (leaf == pending.end()) throw std::logic_error("dependencies: cyclic stop plan");
        plan.push_back(*leaf); pending.erase(leaf);
    }
    return plan;
}
// 逐阶段停止具体实例，成功退出者归还出向依赖，失败不强制释放或回滚已完成步骤。
// 步骤：校验策略并按默认规则拒绝 → 冻结联动计划 → 逐项停止 → 只清理已退出者关系或报告阻挡项。
DependencyShutdown PluginDependencies::shutdown(const std::shared_ptr<PluginInstance>& provider, DependencyStop mode) {
    if (!provider || (mode != DependencyStop::refuse_dependents && mode != DependencyStop::linked))
        throw std::invalid_argument("dependencies: provider and valid stop mode required");
    if (mode == DependencyStop::refuse_dependents) {
        for (const auto handle : slots_) if (!handle.empty()) {
            const auto& binding = references_.get(handle).binding;
            if (binding->provider() == provider) return {ShutdownResult::in_use,binding->consumer(),0};
        }
    }
    const auto plan = mode == DependencyStop::linked ? stop_plan(provider) : std::vector<std::shared_ptr<PluginInstance>>{provider};
    std::size_t closed = 0;
    for (const auto& instance : plan) {
        const bool already_closed = instance->phase() == InstancePhase::unloaded;
        const auto result = stop_instance(instance);
        if (result != ShutdownResult::closed) return {result,instance,closed};
        if (!already_closed) ++closed;
    }
    return {ShutdownResult::closed,{},closed};
}
// 实例实际退出后才归还出向绑定，失败和正常等待均保持依赖保护。
ShutdownResult PluginDependencies::stop_instance(const std::shared_ptr<PluginInstance>& instance) {
    const auto result = instance->shutdown();
    if (result == ShutdownResult::closed)
        for (auto& handle : slots_) if (!handle.empty() && references_.get(handle).binding->consumer() == instance) {
            references_.release(handle); handle = {};
        }
    return result;
}
// 合并相交依赖闭包，防止一个失败消费者被不同提供方在同一轮反复停止。
// 步骤：校验根集合 → 准备去重且消费者优先的计划 → 预留回执 → 各尝试一次并保留独立结果。
std::vector<InstanceShutdown> PluginDependencies::shutdown_all(std::span<const std::shared_ptr<PluginInstance>> providers) {
    if (providers.size() > PluginInstance::maximum_instances) throw std::length_error("dependencies: too many stop roots");
    std::vector<std::shared_ptr<PluginInstance>> plan;
    for (const auto& provider : providers) {
        if (!provider) throw std::invalid_argument("dependencies: stop root required");
        for (const auto& instance : stop_plan(provider))
            if (std::find(plan.begin(),plan.end(),instance) == plan.end()) plan.push_back(instance);
    }
    std::vector<InstanceShutdown> results; results.reserve(plan.size());
    for (const auto& instance : plan) results.push_back({instance,stop_instance(instance)});
    return results;
}
}
