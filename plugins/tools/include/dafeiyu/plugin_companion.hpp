#pragma once
#include <dafeiyu/plugin_instance.hpp>
#include <dafeiyu/tool_catalog.hpp>

namespace dfy::plugins {
struct CompanionOffer {
    runtime::DomainId generation = 0;
    std::string state_tool, state_topic;
};
// 从插件的伙伴能力声明取得可交给模型的工具，核对同实例注册及唯一性。
std::vector<std::string> companion_skills(PluginInstance& instance, const agent::ToolCatalog& catalog);
// 从当前实例复制陪伴声明，核对只读工具注册后返回纯值，旧插件返回空值。
std::optional<CompanionOffer> companion_offer(PluginInstance& instance, const agent::ToolCatalog& catalog);
// 读取可选业务用途，核对具体注册实例及预期的只读或变更类别。
std::optional<std::string> companion_route(PluginInstance& instance, const agent::ToolCatalog& catalog,
    std::string_view purpose, bool changes_state);
}
