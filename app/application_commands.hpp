#pragma once
#include <iosfwd>
#include <string_view>

namespace dfy::app {
class Application;
// 输出当前终端的通用 ASK，并记录真实呈现。
void present_console_ask(Application& application, std::ostream& output);

// 分派终端的显式运行和插件命令，未知命令返回 false。
bool application_command(Application& application, std::string_view command, std::ostream& output, std::ostream& errors);
}
