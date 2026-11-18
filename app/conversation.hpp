#pragma once
#include <dafeiyu/session.hpp>
#include <exception>

namespace dfy::app {
struct CompletedTurn {
    agent::TurnDiagnostics status;
    std::string answer;
    std::exception_ptr error;
};

// 在宿主线程接纳问答并提供值快照，实际退出后归还回合引用。
class Conversation {
  public:
    // 借用同一宿主线程中寿命更长的会话。
    explicit Conversation(agent::Session& session) noexcept : session_(session) {}
    // 只由原服务承担回合输出和实际退出责任。
    Conversation(const Conversation&) = delete;
    // 禁止替换已经绑定的会话。
    Conversation& operator=(const Conversation&) = delete;
    // 按应用宿主提供的范围接纳运行并返回本轮开始状态。
    agent::TurnDiagnostics submit(const std::string& prompt, agent::RunOptions options = {});
    // 向尚待结算的回合登记用户取消。
    bool cancel();
    // 空闲时清空会话上下文，返回原通知回执。
    events::ObservationReceipt clear() { return session_.clear(); }
    // 查询当前或最近回合，不推进会话。
    std::optional<agent::TurnDiagnostics> status() const noexcept;
    // 推进原会话，首次结算时返回值结果，实际退出后保留最近状态。
    std::optional<CompletedTurn> advance();
    // 停止接纳并请求当前会话取消，后续仍须推进实际退出。
    void close() { session_.close(); }
    // 查询是否还有未退出的会话工作。
    bool busy() const noexcept { return session_.busy(); }
    // 确认会话关闭且本服务已经结算退出。
    bool closed() const noexcept { return session_.closed() && !current_; }
    // 返回原会话身份供界面关联本地回执。
    runtime::DomainId id() const noexcept { return session_.id(); }
  private:
    agent::Session& session_;
    std::shared_ptr<agent::TurnRequest> current_;
    std::optional<agent::TurnDiagnostics> last_;
    bool reported_ = false;
};
}
