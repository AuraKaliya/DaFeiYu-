#pragma once
#include "chat.hpp"
#include <dafeiyu/line_input.hpp>

namespace dfy::app {
using HostPump = std::function<void()>;
// 轮询输入与会话，持续推进宿主事件；输入关闭和业务结果均不能提前退出资源回收。
int async_chat(Conversation& conversation, platform::LineInput& input, std::ostream& output, std::ostream& errors,
    HostPump pump = {}, LocalCommand command = {}, ChatSubmit submit = {});
}
