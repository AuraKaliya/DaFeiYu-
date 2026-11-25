#include "async_chat.hpp"
#include <iostream>
#include <thread>

namespace dfy::app {
// 轮询适配器和宿主工作，不在模型请求或终端读调用上阻塞事件推进。
// 步骤：展示操作提示 → 交替推进宿主与会话 → 分派有界输入 → 关闭输入并等待实际退出 → 返回输出状态。
int async_chat(Conversation& conversation, platform::LineInput& input, std::ostream& output, std::ostream& errors,
    HostPump pump, LocalCommand command, ChatSubmit submit) {
    ChatController controller(conversation,output,errors,std::move(command),std::move(submit)); bool input_failed = false, input_closing = false;
    output << "DaFeiYu\nEnter a message, /status, /clear, /cancel or /exit. Local commands remain available while waiting.\n" << std::flush;
    while (!controller.done() || !input.exited()) {
        if (pump) pump(); controller.advance();
        if (!controller.closing()) {
            auto read = input.poll();
            switch (read.state) {
            case platform::InputState::line:
                controller.line(std::move(read.line)); output << std::flush; errors << std::flush; break;
            case platform::InputState::end: controller.finish_input(); break;
            case platform::InputState::failed:
                errors << "Input failed: " << read.error << '\n'; input_failed = true; controller.request_exit(); break;
            case platform::InputState::empty: break;
            }
        }
        if (!output || !errors) { input_failed = true; controller.request_exit(); }
        if (controller.closing() && !input_closing) { input.close(); input_closing = true; }
        input.advance();
        if (!controller.done() || !input.exited()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (pump) pump();
    return input_failed || !output || !errors ? 1 : 0;
}
}
