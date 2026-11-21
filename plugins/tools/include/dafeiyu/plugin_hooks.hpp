#pragma once
#include <dafeiyu/run_options.hpp>
#include <dafeiyu/plugin_manager.hpp>

namespace dfy::plugins {
// 为指定会话连接固定的控制表，零所有者表示本体主会话。
agent::PolicyExtension plugin_controls(const PluginHookSnapshot& snapshot, runtime::DomainId owner = 0);
// 为指定会话连接同一快照的字段转换表。
agent::TextHook plugin_text_hooks(const PluginHookSnapshot& snapshot, runtime::DomainId owner = 0);
// 把实际退出事实交给本轮固定的观察表。
agent::TurnExitSink plugin_exit_hook(const PluginHookSnapshot& snapshot);
}
