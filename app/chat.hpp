#pragma once
#include <dafeiyu/async_session.hpp>
#include <dafeiyu/line_input.hpp>
#include <functional>
#include <iosfwd>

namespace dfy::app {
using LocalCommand = std::function<bool(std::string_view)>;
using HostPump = std::function<void()>;
// 同宿主线程的终端交互状态，问答等待不阻挡取消或本地管理命令。
class ChatController {
  public:
    // 借用会话与输出，管理命令处理器只在宿主线程调用。
    ChatController(agent::AsyncSession& session, std::ostream& output, std::ostream& errors, LocalCommand command = {});
    // 分派输入；忙碌时普通问答明确拒绝，本地管理及取消仍可使用。
    void line(std::string text);
    // 推进会话并输出每轮唯一结果，实际退出前继续保留该回合。
    void advance();
    // 输入正常结束时停止新输入，保留当前问答直到正常完成。
    void finish_input() noexcept { ending_ = true; }
    // 明确请求退出时取消当前问答，真实网络资源仍由会话继续回收。
    void request_exit();
    // 判断是否停止接收后续输入。
    bool closing() const noexcept { return ending_; }
    // 判断会话及当前回合已实际结束。
    bool done() const noexcept { return ending_ && session_.closed() && !current_; }
  private:
    agent::AsyncSession& session_;
    std::ostream& output_;
    std::ostream& errors_;
    LocalCommand command_;
    std::shared_ptr<agent::TurnRequest> current_;
    bool reported_ = false, ending_ = false;
};
// 轮询输入与会话，持续推进宿主事件；输入关闭和业务结果均不能提前退出资源回收。
int async_chat(agent::AsyncSession& session, platform::LineInput& input, std::ostream& output, std::ostream& errors,
    HostPump pump = {}, LocalCommand command = {});
}
