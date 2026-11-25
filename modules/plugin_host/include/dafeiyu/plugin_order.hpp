#pragma once
#include <span>
#include <string_view>
#include <vector>

namespace dfy::plugins {
struct PluginOrderNode {
    std::string_view id;
    std::span<const std::string_view> dependencies;
};

// 按输入注册顺序选择可运行项，返回节点索引；未知依赖、重复 ID 或环均拒绝。
std::vector<std::size_t> stable_plugin_order(std::span<const PluginOrderNode> registered);
}
