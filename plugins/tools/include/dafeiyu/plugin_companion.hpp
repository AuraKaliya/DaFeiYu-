#pragma once
#include <dafeiyu/plugin_instance.hpp>
#include <dafeiyu/tool_catalog.hpp>

namespace dfy::plugins {
struct CompanionOffer {
    runtime::DomainId generation = 0;
    std::string state_tool, state_topic;
};
// 从当前实例复制陪伴声明，核对只读工具注册后返回纯值，旧插件返回空值。
std::optional<CompanionOffer> companion_offer(PluginInstance& instance, const agent::ToolCatalog& catalog);
// 读取可选业务用途，核对具体注册实例及预期的只读或变更类别。
std::optional<std::string> companion_route(PluginInstance& instance, const agent::ToolCatalog& catalog,
    std::string_view purpose, bool changes_state);
}
