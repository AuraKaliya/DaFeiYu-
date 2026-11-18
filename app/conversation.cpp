#include "conversation.hpp"

namespace dfy::app {
using runtime::OperationState;

// 接纳问答并固定唯一回合，拒绝覆盖尚待退出的工作。
// 步骤：核对当前回合 → 通过原会话接纳 → 返回开始状态。
agent::TurnDiagnostics Conversation::submit(const std::string& prompt) {
    if (current_) throw std::logic_error("session: wait for the current request to finish");
    current_ = session_.begin(prompt);
    reported_ = false;
    return current_->diagnostics();
}

// 仅向当前待结算回合请求协作取消。
bool Conversation::cancel() {
    if (!current_ || current_->operation()->state() != OperationState::pending) return false;
    current_->request_cancel(runtime::CancelReason::user);
    return true;
}

// 返回当前回合或最近一次实际退出的诊断值。
std::optional<agent::TurnDiagnostics> Conversation::status() const noexcept {
    if (current_) return current_->diagnostics();
    return last_;
}

// 返回一次业务结算，同时继续跟踪实际退出。
// 步骤：推进原会话 → 提取首次结算的结果或错误 → 实际退出后保存值状态并归还回合。
std::optional<CompletedTurn> Conversation::advance() {
    session_.advance();
    if (!current_) return {};

    const auto status = current_->diagnostics();
    std::optional<CompletedTurn> completed;
    if (!reported_ && status.result != OperationState::pending) {
        completed.emplace(CompletedTurn{status,{},{}});
        if (status.result != OperationState::cancelled) {
            try { completed->answer = current_->answer(); }
            catch (...) { completed->error = std::current_exception(); }
        }
        reported_ = true;
    }

    if (status.exit != OperationState::pending) {
        last_ = status;
        current_.reset();
    }
    return completed;
}
}
