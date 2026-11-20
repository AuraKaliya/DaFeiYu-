#pragma once
#include <dafeiyu/execution_policy.hpp>
#include <dafeiyu/plugin_manager.hpp>

namespace dfy::plugins {
// 由寿命更长的插件管理器裁定 Agent 控制点；零所有者表示本体主会话。
agent::PolicyExtension plugin_controls(PluginManager& manager, runtime::DomainId owner = 0);
}
