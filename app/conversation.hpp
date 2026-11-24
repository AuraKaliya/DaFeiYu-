#pragma once
#include <dafeiyu/session.hpp>
#include <dafeiyu/diagnostic_snapshot.hpp>
#include <exception>
#include <vector>

namespace dfy::plugins { class PluginManager; }
namespace dfy::app {
inline constexpr std::size_t dialogue_display_byte_limit = 2*1024*1024;
struct PersonaProfile;
struct CompletedTurn {
    agent::TurnDiagnostics status;
    std::string answer;
    std::exception_ptr error;
};
// 仅供受信界面读取的本进程对话，使用正式会话签发的回合身份。
struct DisplayedTurn {
    agent::RunId run;
    std::string prompt, answer;
    runtime::OperationState result = runtime::OperationState::pending, exit = runtime::OperationState::pending;
    std::uint64_t queued_message = 0;
    bool accepts_input = false;
    std::uint64_t input_revision = 0;
    std::vector<agent::TurnInput> inputs;
};
struct QueuedDialogue {
    std::uint64_t message;
    runtime::DomainId session;
    std::string text;
};
struct ConversationDisplay {
    std::uint64_t revision = 0;
    std::vector<DisplayedTurn> turns;
    std::vector<QueuedDialogue> queue;
    bool queue_paused = false;
};

// 在宿主线程接纳问答并提供值快照，实际退出后归还回合引用。
class Conversation {
  public:
    // 借用寿命更长的会话和可选插件管理器，并保存应用构建标签。
    explicit Conversation(agent::Session& session, agent::RunVersionLabels version_labels = {}, plugins::PluginManager* plugins = nullptr,
        const PersonaProfile* persona = nullptr) noexcept
        : session_(session), version_labels_(std::move(version_labels)), plugins_(plugins), persona_(persona) {}
    // 只由原服务承担回合输出和实际退出责任。
    Conversation(const Conversation&) = delete;
    // 禁止替换已经绑定的会话。
    Conversation& operator=(const Conversation&) = delete;
    // 接纳运行并返回开始状态，显式提供用户原话时才保留显示条目。
    agent::TurnDiagnostics submit(const std::string& prompt, agent::RunOptions options = {}, bool player_facing = false,
        const std::string* display_prompt = nullptr);
    // 向尚待结算的回合登记用户取消。
    bool cancel();
    // 空闲时清空会话上下文，返回原通知回执。
    events::ObservationReceipt clear();
    // 业务线程借用受信对话值，跨线程调用方须按修订复制快照。
    const ConversationDisplay& display() const noexcept { return display_; }
    // 查询当前或最近回合，不推进会话。
    std::optional<agent::TurnDiagnostics> status() const noexcept;
    // 获取当前或最近回合的完整无正文诊断值。
    std::optional<agent::DiagnosticSnapshot> diagnostic() const;
    // 推进原会话，首次结算时返回值结果，实际退出后保留最近状态。
    std::optional<CompletedTurn> advance();
    // 停止接纳并请求当前会话取消，后续仍须推进实际退出。
    void close() { session_.close(); }
    // 查询是否还有未退出的会话工作。
    bool busy() const noexcept { return session_.busy(); }
    // 返回是否可继续接纳新回合。
    bool accepting() const noexcept { return session_.accepting(); }
    // 确认会话关闭且本服务已经结算退出。
    bool closed() const noexcept { return session_.closed() && !current_; }
    // 返回原会话身份供界面关联本地回执。
    runtime::DomainId id() const noexcept { return session_.id(); }
  private:
    // 同线程应用宿主可读取原回合以裁定受控命令。
    friend class Application;
    friend class CompanionSession;
    // 构造当前会话命名空间并从原回合提取诊断。
    agent::DiagnosticSnapshot capture_diagnostic() const;
    // 用原回合的结算与退出更新最后一条受信对话。
    void update_display(const agent::TurnDiagnostics& status, const std::optional<CompletedTurn>& completed);
    // 按条目数和正文预算从最早的条目收回显示记录。
    void trim_display() noexcept;

    agent::Session& session_;
    const agent::RunVersionLabels version_labels_;
    plugins::PluginManager* const plugins_;
    const PersonaProfile* const persona_;
    std::shared_ptr<agent::TurnRequest> current_;
    std::optional<agent::TurnDiagnostics> last_;
    std::optional<agent::DiagnosticSnapshot> last_diagnostic_;
    std::exception_ptr diagnostic_error_;
    mutable std::string diagnostic_namespace_;
    ConversationDisplay display_;
    std::size_t display_bytes_ = 0;
    bool reported_ = false;
};
}
