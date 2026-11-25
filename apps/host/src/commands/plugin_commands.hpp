#pragma once
#include <dafeiyu/plugin_manager.hpp>
#include <iosfwd>
#include <string_view>

namespace dfy::app {
// 按空白与双引号拆分本地命令，保留 Windows 反斜杠，最多八个参数及 64 KiB 输入。
std::vector<std::string> command_words(std::string_view command);
// 处理本地插件目录、安装及实例命令，未知命令族返回 false，处理错误独立输出。
bool plugin_command(plugins::PluginManager& manager, std::string_view command, std::ostream& output, std::ostream& errors);
}
