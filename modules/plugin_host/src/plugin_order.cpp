#include <dafeiyu/plugin_order.hpp>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace dfy::plugins {
namespace {
// 从尚未排定的节点沿残余依赖找出一条具体的环。
// 步骤：追踪未排定的提供方 → 找到首次重复节点 → 格式化完整环路。
std::string dependency_cycle(std::span<const PluginOrderNode> nodes,
    const std::vector<std::vector<std::size_t>>& prerequisites, const std::vector<std::size_t>& incoming) {
    const auto pending = [&](std::size_t count) { return count <= nodes.size(); };
    auto next = static_cast<std::size_t>(std::ranges::find_if(incoming,pending) - incoming.begin());
    std::vector<std::size_t> path;
    auto repeated = path.end();
    while (repeated == path.end()) {
        path.push_back(next);
        next = *std::ranges::find_if(prerequisites[next],[&](auto source) { return pending(incoming[source]); });
        repeated = std::ranges::find(path,next);
    }
    std::string error = "plugin dependency cycle: ";
    for (; repeated != path.end(); ++repeated) { error += nodes[*repeated].id; error += " -> "; }
    error += nodes[next].id;
    return error;
}
}

// 以提供方优先和注册顺序构造可复用的完整执行计划。
// 步骤：校验并索引注册项 → 连接依赖边和反向关系 → 取最早就绪项 → 报告循环或返回方案。
std::vector<std::size_t> stable_plugin_order(std::span<const PluginOrderNode> registered) {
    const auto count = registered.size();
    std::unordered_map<std::string_view,std::size_t> indices;
    indices.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
        if (registered[index].id.empty() || !indices.emplace(registered[index].id,index).second)
            throw std::invalid_argument("plugin order: empty or duplicate id");

    std::vector<std::vector<std::size_t>> prerequisites(count), dependents(count);
    std::vector<std::size_t> incoming(count);
    for (std::size_t target = 0; target < count; ++target) {
        auto& providers = prerequisites[target];
        for (const auto dependency : registered[target].dependencies) {
            const auto source = indices.find(dependency);
            if (source == indices.end()) throw std::invalid_argument("missing plugin dependency: "+std::string(dependency));
            if (std::ranges::find(providers,source->second) != providers.end()) continue;
            providers.push_back(source->second);
            dependents[source->second].push_back(target);
        }
        incoming[target] = providers.size();
    }

    std::vector<std::size_t> result;
    result.reserve(count);
    while (result.size() < count) {
        const auto first = std::ranges::find(incoming,0);
        if (first == incoming.end()) throw std::logic_error(dependency_cycle(registered,prerequisites,incoming));
        const auto index = static_cast<std::size_t>(first - incoming.begin());
        *first = count + 1;
        result.push_back(index);
        for (const auto dependent : dependents[index]) --incoming[dependent];
    }
    return result;
}
}
