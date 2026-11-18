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
}
