#pragma once
#include <iosfwd>
#include <string_view>

namespace dfy::app {
class Application;

// 分派终端的显式运行和插件命令，未知命令返回 false。
bool application_command(Application& application, std::string_view command, std::ostream& output, std::ostream& errors);
}
