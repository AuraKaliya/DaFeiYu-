#include "chat.hpp"
#include "diagnostics.hpp"
#include <iostream>

namespace dfy::app {
// 固定宿主会话与输出责任，不在构造阶段发起请求。
ChatController::ChatController(Conversation& conversation, std::ostream& output, std::ostream& errors, LocalCommand command, ChatSubmit submit)
    : conversation_(conversation), output_(output), errors_(errors), command_(std::move(command)), submit_(std::move(submit)) {}
// 统一解释控制台和管道输入的 Windows 行尾。
std::string_view terminal_line(std::string_view text) noexcept {
    return text.substr(0,text.find_last_not_of('\r')+1);
}
// 管理命令不发送给模型，普通输入只在会话实际空闲时接纳。
// 步骤：忽略结束和空白 → 处理退出、取消、清空及诊断 → 转交本地管理 → 接受问答或独立报告拒绝。
void ChatController::line(std::string text, ChatExit on_exit) {
    if (ending_) return;
    text.resize(terminal_line(text).size());
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) return;
    try {
        if (text == "/exit") {
            if (on_exit==ChatExit::close_session) request_exit();
            else ending_ = true;
            return;
        }
        if (text == "/status") {
            if (const auto status = conversation_.status()) write_turn_status(output_,*status);
            else output_ << "No accepted question in this session.\n";
            return;
        }
        if (text == "/cancel") {
            if (!conversation_.cancel()) output_ << "No active request to cancel.\n";
            return;
        }
        if (text == "/clear") {
            const auto receipt = conversation_.clear(); output_ << "Context cleared.\n";
            write_observation_warning(errors_,conversation_.id(),0,"session_cleared",receipt); return;
        }
        if (text.starts_with('/')) {
            if (!command_ || !command_(text)) errors_ << "Unknown command. Use /status, /clear, /cancel or /exit.\n";
            return;
        }
        const auto status = submit_ ? submit_(text) : conversation_.submit(text);
        write_observation_warning(errors_,status.session,status.turn,"turn_started",*status.notifications.started);
    } catch (const std::exception& error) { errors_ << error.what() << '\n'; }
    catch (...) { errors_ << "Command failed.\n"; }
}
// 由原回合生成可交付到本地或连接终端的相同输出。
// 步骤：核对终态通知 → 按取消、回答或错误输出 → 刷新两个输出通道。
void write_chat_result(const CompletedTurn& completed, std::ostream& output, std::ostream& errors, bool include_answer) {
    const auto& status = completed.status;
    if (status.notifications.finished) {
        const auto kind = status.result == runtime::OperationState::succeeded ? "turn_completed" :
            status.result == runtime::OperationState::cancelled ? "turn_cancelled" : "turn_failed";
        write_observation_warning(errors,status.session,status.turn,kind,*status.notifications.finished);
    }
    if (status.result == runtime::OperationState::cancelled) {
        output << "Request cancelled.";
        if (status.exit == runtime::OperationState::pending) output << " Waiting for the current call to finish.";
        output << '\n';
    }
    else {
        try {
            if (completed.error) std::rethrow_exception(completed.error);
            if (include_answer) output << completed.answer << '\n';
        }
        catch (const std::exception& error) { errors << error.what() << '\n'; }
        catch (...) { errors << "Model request failed.\n"; }
    }
    output << std::flush; errors << std::flush;
}
// 展示应用会话交付的唯一结果，正常 EOF 等待实际退出后关闭。
// 步骤：推进会话并输出结算 → 在 EOF 时处理等待确认及关闭。
void ChatController::advance() {
    if (const auto completed = conversation_.advance()) write_chat_result(*completed,output_,errors_);
    if (ending_) {
        if (!conversation_.busy()) conversation_.close();
        else if (const auto status = conversation_.status(); status && status->waiting_confirmations) conversation_.cancel();
    }
}
// 明确退出请求停止新问答并协作取消当前调用，完成确认仍由 advance 处理。
void ChatController::request_exit() { ending_ = true; conversation_.close(); }
}
