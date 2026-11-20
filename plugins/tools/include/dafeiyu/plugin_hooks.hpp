#pragma once
#include <dafeiyu/run_options.hpp>
#include <dafeiyu/plugin_manager.hpp>

namespace dfy::plugins {
// 由寿命更长的插件管理器裁定 Agent 控制点；零所有者表示本体主会话。
agent::PolicyExtension plugin_controls(PluginManager& manager, runtime::DomainId owner = 0);
// 为指定会话连接插件管理器中已发布的字段转换表。
agent::TextHook plugin_text_hooks(PluginManager& manager, runtime::DomainId owner = 0);
}
