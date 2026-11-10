#pragma once
#include <dafeiyu/session.hpp>
#include <iosfwd>

namespace dfy::app {
// 从 UTF-8 输入逐行驱动会话，正常退出返回 0，输入输出故障返回 1。
int chat(agent::Session& session, std::istream& input, std::ostream& output, std::ostream& errors);
}
