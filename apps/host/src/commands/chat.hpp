#pragma once
#include "conversation.hpp"
#include <functional>
#include <iosfwd>

namespace dfy::app {
using LocalCommand = std::function<bool(std::string_view)>;
using ChatSubmit = std::function<agent::TurnDiagnostics(const std::string&)>;
enum class ChatExit { close_session, detach };
// 返回去掉全部终端行尾回车的视图，保留其他正文原样。
std::string_view terminal_line(std::string_view text) noexcept;
// 按本轮真实结算分别写出终端正文、取消状态、模型失败与观察回执。
void write_chat_result(const CompletedTurn& completed, std::ostream& output, std::ostream& errors, bool include_answer = true);
// 同宿主线程的终端交互状态，问答等待不阻挡取消或本地管理命令。
class ChatController {
  public:
    // 借用会话与输出，管理命令处理器只在宿主线程调用。
    ChatController(Conversation& conversation, std::ostream& output, std::ostream& errors, LocalCommand command = {}, ChatSubmit submit = {});
    // 分派输入；忙碌时普通问答明确拒绝，本地管理及取消仍可使用。
    void line(std::string text, ChatExit on_exit = ChatExit::close_session);
    // 推进会话并输出每轮唯一结果，实际退出前继续保留该回合。
    void advance();
    // 输入正常结束时停止新输入，保留获准工作并结束仍待人工确认的运行。
    void finish_input() noexcept { ending_ = true; }
    // 明确请求退出时取消当前问答，真实网络资源仍由会话继续回收。
    void request_exit();
    // 判断是否停止接收后续输入。
    bool closing() const noexcept { return ending_; }
    // 判断会话及当前回合已实际结束。
    bool done() const noexcept { return ending_ && conversation_.closed(); }
  private:
    Conversation& conversation_;
    std::ostream& output_;
    std::ostream& errors_;
    LocalCommand command_;
    ChatSubmit submit_;
    bool ending_ = false;
};
}
