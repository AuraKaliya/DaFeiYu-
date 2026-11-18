#include "chat.hpp"
#include "diagnostics.hpp"
#include <iostream>
#include <thread>

namespace dfy::app {
// 固定宿主会话与输出责任，不在构造阶段发起请求。
ChatController::ChatController(Conversation& conversation, std::ostream& output, std::ostream& errors, LocalCommand command)
    : conversation_(conversation), output_(output), errors_(errors), command_(std::move(command)) {}
// 管理命令不发送给模型，普通输入只在会话实际空闲时接纳。
// 步骤：忽略结束和空白 → 处理退出、取消、清空及诊断 → 转交本地管理 → 接受问答或独立报告拒绝。
void ChatController::line(std::string text) {
    if (ending_) return;
    if (text.ends_with('\r')) text.pop_back();
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) return;
    try {
        if (text == "/exit") { request_exit(); return; }
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
        const auto status = conversation_.submit(text);
        write_observation_warning(errors_,status.session,status.turn,"turn_started",*status.notifications.started);
    } catch (const std::exception& error) { errors_ << error.what() << '\n'; }
    catch (...) { errors_ << "Command failed.\n"; }
}
// 展示应用会话交付的唯一结果，正常 EOF 等待实际退出后关闭。
// 步骤：推进应用会话 → 同步终态通知和业务结果 → 在 EOF 时处理等待确认及关闭。
void ChatController::advance() {
    if (const auto completed = conversation_.advance()) {
        const auto& status = completed->status;
        if (status.notifications.finished) {
            const auto kind = status.result == runtime::OperationState::succeeded ? "turn_completed" :
                status.result == runtime::OperationState::cancelled ? "turn_cancelled" : "turn_failed";
            write_observation_warning(errors_,status.session,status.turn,kind,*status.notifications.finished);
        }
        if (status.result == runtime::OperationState::cancelled) {
            output_ << "Request cancelled.";
            if (status.exit == runtime::OperationState::pending) output_ << " Waiting for the current call to finish.";
            output_ << '\n';
        }
        else {
            try {
                if (completed->error) std::rethrow_exception(completed->error);
                output_ << completed->answer << '\n';
            }
            catch (const std::exception& error) { errors_ << error.what() << '\n'; }
            catch (...) { errors_ << "Model request failed.\n"; }
        }
        output_ << std::flush; errors_ << std::flush;
    }
    if (ending_) {
        if (!conversation_.busy()) conversation_.close();
        else if (const auto status = conversation_.status(); status && status->waiting_confirmations) conversation_.cancel();
    }
}
// 明确退出请求停止新问答并协作取消当前调用，完成确认仍由 advance 处理。
void ChatController::request_exit() { ending_ = true; conversation_.close(); }
// 轮询适配器和宿主工作，不在模型请求或终端读调用上阻塞事件推进。
// 步骤：展示操作提示 → 交替推进宿主与会话 → 分派有界输入 → 关闭输入并等待实际退出 → 返回输出状态。
int async_chat(Conversation& conversation, platform::LineInput& input, std::ostream& output, std::ostream& errors,
    HostPump pump, LocalCommand command) {
    ChatController controller(conversation,output,errors,std::move(command)); bool input_failed = false, input_closing = false;
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
