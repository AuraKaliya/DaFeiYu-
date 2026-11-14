#include "chat.hpp"
#include <iostream>
#include <thread>

namespace dfy::app {
// 固定宿主会话与输出责任，不在构造阶段发起请求。
ChatController::ChatController(agent::AsyncSession& session, std::ostream& output, std::ostream& errors, LocalCommand command)
    : session_(session), output_(output), errors_(errors), command_(std::move(command)) {}
// 管理命令不发送给模型，普通输入只在会话实际空闲时接纳。
// 步骤：忽略结束和空白 → 处理退出、取消与清空 → 转交本地管理 → 接受问答或独立报告拒绝。
void ChatController::line(std::string text) {
    if (ending_) return;
    if (text.ends_with('\r')) text.pop_back();
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) return;
    try {
        if (text == "/exit") { request_exit(); return; }
        if (text == "/cancel") {
            if (current_ && current_->operation()->state() == runtime::OperationState::pending) current_->request_cancel(runtime::CancelReason::user);
            else output_ << "No active request to cancel.\n";
            return;
        }
        if (text == "/clear") { session_.clear(); output_ << "Context cleared.\n"; return; }
        if (text.starts_with('/')) {
            if (!command_ || !command_(text)) errors_ << "Unknown command. Use /clear, /cancel or /exit.\n";
            return;
        }
        if (current_) throw std::logic_error("session: wait for the current request to finish");
        current_ = session_.begin(text); reported_ = false;
    } catch (const std::exception& error) { errors_ << error.what() << '\n'; }
    catch (...) { errors_ << "Command failed.\n"; }
}
// 输出业务结果后继续跟踪后台退出，正常 EOF 不取消尚未完成的回答。
// 步骤：推进会话 → 输出唯一业务结果 → 归还退出回合 → 按 EOF 状态关闭空闲会话。
void ChatController::advance() {
    session_.advance();
    if (current_ && !reported_ && current_->operation()->state() != runtime::OperationState::pending) {
        reported_ = true;
        if (current_->operation()->state() == runtime::OperationState::cancelled) output_ << "Request cancelled. Waiting for the current call to finish.\n";
        else {
            try { output_ << current_->answer() << '\n'; }
            catch (const std::exception& error) { errors_ << error.what() << '\n'; }
            catch (...) { errors_ << "Model request failed.\n"; }
        }
        output_ << std::flush; errors_ << std::flush;
    }
    if (current_ && current_->exit()->state() != runtime::OperationState::pending) current_.reset();
    if (ending_ && !session_.busy()) session_.close();
}
// 明确退出请求停止新问答并协作取消当前调用，完成确认仍由 advance 处理。
void ChatController::request_exit() { ending_ = true; session_.close(); }
// 轮询适配器和宿主工作，不在模型请求或终端读调用上阻塞事件推进。
// 步骤：展示操作提示 → 交替推进宿主与会话 → 分派有界输入 → 关闭输入并等待实际退出 → 返回输出状态。
int async_chat(agent::AsyncSession& session, platform::LineInput& input, std::ostream& output, std::ostream& errors,
    HostPump pump, LocalCommand command) {
    ChatController controller(session,output,errors,std::move(command)); bool input_failed = false, input_closing = false;
    output << "DaFeiYu\nEnter a message, /clear, /cancel or /exit. Local commands remain available while waiting.\n" << std::flush;
    while (!controller.done() || !input.exited()) {
        if (pump) pump(); controller.advance();
        if (!controller.closing()) {
            auto read = input.poll();
            switch (read.state) {
            case platform::InputState::line: controller.line(std::move(read.line)); break;
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
