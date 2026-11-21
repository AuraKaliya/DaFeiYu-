#pragma once
#include "conversation.hpp"
#include "companion.hpp"
#include "tool_verification.hpp"
#include "subagents.hpp"
#include "mailbox_intake.hpp"
#include "interaction_routes.hpp"
#include <dafeiyu/plugin_manager.hpp>
#include <dafeiyu/plugin_tools.hpp>
#include <array>

namespace dfy::app {
class RunLog;
struct VerificationReport {
    agent::RunId run;
    std::optional<agent::ObservationMatch> match;
};
// 同一运行的无正文状态与读回判定，可在宿主线程按原身份查询。
struct RunStatusSnapshot {
    agent::TurnDiagnostics status;
    std::optional<VerificationReport> verification;
};
struct TaskAdmission {
    agent::TurnDiagnostics status;
    bool reused = false;
};
struct ApplicationRunConfig {
    agent::ToolClock tool_clock = std::chrono::steady_clock::now;
    std::optional<std::string> model_profile;
    bool wake_for_room_address = true;
    bool wake_for_plugin_mail = true;
    std::filesystem::path trace_directory;
};
enum class RoomEvent { presence_changed, addressed };

// 在一个宿主线程上组装会话、插件和共享事件，调用方负责定期推进。
class Application {
  public:
    // 创建使用内置结构化 Provider 的独立应用实例。
    Application(model::Config config, const std::filesystem::path& packages, const std::filesystem::path& user_data);
    // 以显式后端创建应用，可为工具运行指定统一时钟及公开模型档案。
    Application(model::Config config, const std::filesystem::path& packages, const std::filesystem::path& user_data,
        model::Backend backend, ApplicationRunConfig run = {});
    // 返回本次宿主运行的本机日志位置，未开启时返回空路径。
    std::filesystem::path trace_file() const;
    // 访问应用拥有的问答与运行服务。
    Conversation& conversation() noexcept { return conversation_; }
    // 使用本体的直接对话信箱接纳一轮，保留普通聊天的原正文和回执。
    agent::TurnDiagnostics submit_dialogue(const std::string& text);
    // 受信房间服务把已知事实投递到单独地址，接纳回执独立于主回合。
    events::DeliveryReceipt post_room_event(RoomEvent event, const std::string& room, const std::string& text);
    // 在当前主会话允许时主动审查一封房间来信。
    MailboxIntakeResult review_room();
    // 在原宿主线程使用应用拥有的插件命令入口。
    plugins::PluginManager& plugins() noexcept { return plugins_; }
    // 借用本体持有的信箱目录及主体身份供宿主入口使用。
    const plugins::PluginMailboxHost& mailbox_host() const noexcept { return mailbox_host_; }
    // 借用宿主的插件子会话服务。
    Subagents& subagents() noexcept { return *subagents_; }
    // 提供可信入口配置主会话按分类收件与发送方审查。
    MailboxIntake& mailbox_intake() noexcept { return mailbox_intake_; }
    // 根据已加载实例的可选角色声明启用一个游戏会话。
    runtime::DomainId open_companion(const std::string& package);
    // 在原陪伴会话内提交用户委托。
    agent::TurnDiagnostics submit_companion(runtime::DomainId session, const std::string& prompt);
    // 只停止指定的原陪伴会话。
    void stop_companion(runtime::DomainId session);
    // 复制当前或最近已关闭的陪伴状态。
    std::optional<CompanionStatus> companion_status() const;
    // 提交与模型等待独立的只读状态查询，返回本次或原在途请求身份。
    std::uint64_t refresh_companion(runtime::DomainId session);
    // 按原会话取得最近观察和可用性，不触发查询。
    std::optional<CompanionState> companion_state(runtime::DomainId session) const;
    // 读取最近一次实际状态签发的当前地址。
    std::optional<CompanionAddress> companion_address() const;
    // 允许当前已加载角色接收新玩家消息，从开启后的实际水位开始。
    void listen_companion(runtime::DomainId session, CompanionMode mode = CompanionMode::partner);
    // 返回当前会话内的玩家收件状态。
    std::optional<PlayerInboxStatus> companion_inbox(runtime::DomainId session) const;
    // 读取玩家委托的查看状态及其实际游戏回执。
    std::optional<CompanionVisit> companion_visit(runtime::DomainId session) const;
    // 读取游戏侧和管理中心共用的原问题及状态。
    std::optional<CompanionQuestion> companion_question(runtime::DomainId session) const;
    // 由可信管理入口答复原问题并建立一次采集委托。
    CollectionGrant authorize_companion(const CompanionAddress& address, std::uint64_t question, std::uint64_t revision,
        std::uint64_t command, std::uint64_t quantity);
    // 读取仍保留的采集委托回执。
    std::optional<CollectionGrant> companion_collection(runtime::DomainId session) const;
    // 查询原游戏实际接单或仍待核实的状态。
    std::optional<DeliveryStatus> companion_delivery(runtime::DomainId session) const;
    // 返回原会话最近采掘工作的事实，客户端结束不等于实际入包。
    std::optional<MiningStatus> companion_mining(runtime::DomainId session) const;
    // 在可信主入口询问位置，返回所用最近观察或新读取编号。
    LocationAnswer locate_companion(runtime::DomainId session, LocationPurpose purpose);
    // 查询本局最近的探索介入依据与后续答复。
    std::optional<ExplorationReview> companion_review(runtime::DomainId session) const;
    // 可信入口按原会话和可选任务读取计划板；跨会话查询返回空值。
    std::optional<CompanionBoard> companion_board(runtime::DomainId session, std::optional<std::uint64_t> task = {},
        std::uint64_t after_message = 0) const;
    // 主会话向该地址投递一条委托或询问。
    std::uint64_t send_companion(const CompanionAddress& address, CompanionMessageKind kind,
        std::uint64_t command, std::uint64_t reply_to, const std::string& text);
    // 显式授权主 Agent 询问、委托或读取当前游戏回执。
    agent::TurnDiagnostics submit_companion_exchange(const std::string& prompt);
    // 在可信入口按游标读取主子消息及缺口。
    CompanionDelta companion_messages(runtime::DomainId id, std::uint64_t after) const;
    // 读取仍保留的指定子回合结算值。
    std::optional<CompletedTurn> companion_result(agent::RunId run) const;
    // 宿主主动处理已登记的信箱，等待当前主回合退出。
    MailboxIntakeResult review_mail() { interactions_.prepare_review(); return mailbox_intake_.advance(false); }
    // 取得通用 ASK 的终端问题。
    auto console_ask() const { return interactions_.console_question(); }
    // 由具备呈现与回话能力的入口报告终端当前可用性。
    void set_console_ask_available(bool enabled) noexcept { interactions_.set_terminal_available(enabled); }
    // 提交终端呈现回执或原真人答复。
    bool answer_console_ask(std::uint64_t id, std::optional<std::string> answer) { return interactions_.console_answer(id,std::move(answer)); }
    // 设置插件信箱的自动唤醒模式，主动收件继续可用。
    void wake_for_plugin_mail(bool enabled) noexcept { interactions_.set_auto_wake(enabled); }
    // 显式授予一个已加载实例的只读工具并提交用户目标。
    agent::TurnDiagnostics submit_read_only(const std::string& tool_id, const std::string& prompt);
    // 提交有明确工具范围的用户目标，状态变更进入原会话的人工确认。
    agent::TurnDiagnostics submit_confirmed(const std::string& tool_id, const std::string& prompt);
    // 提交同一实例的无参读取及变更工具，由宿主比对变更后的实际文本状态。
    agent::TurnDiagnostics submit_verified(const std::string& reader, const std::string& change, const std::string& expected,
        const std::string& prompt);
    // 使用当前宿主的顺序请求号提交任务，同一已接纳命令复用原回执。
    TaskAdmission submit_verified_once(std::uint64_t command, const std::string& reader, const std::string& change,
        const std::string& expected, const std::string& prompt);
    // 只读查询仍在保留范围的请求及原运行身份。
    std::optional<agent::RunId> task_run(std::uint64_t command) const noexcept;
    // 读取当前运行或空闲时最近退出的运行，指定身份不匹配时返回空值。
    std::optional<RunStatusSnapshot> run_status(std::optional<agent::RunId> expected = {}) const noexcept;
    // 查询当前或最近一次运行的实际验证判断。
    std::optional<VerificationReport> verification() const noexcept;
    // 为可信界面读取当前运行的确认记录和待审参数。
    std::vector<agent::ConfirmationSnapshot> confirmations() const;
    // 根据当前运行的原请求及期望修订登记决定，同一决定复用原回执。
    agent::ConfirmationReceipt decide(runtime::DomainId request, std::uint64_t revision, bool approve);
    // 回收退出或失效的运行工具绑定，并推进原插件通知和收尾。
    runtime::RunResult advance_plugins();
    // 等原会话实际退出后推进插件关闭，保留管理器的本轮结果。
    plugins::ShutdownResult close_plugins() noexcept;
  private:
    struct ToolRegistration {
        std::uint64_t turn = 0;
        std::weak_ptr<plugins::PluginInstance> instance;
        std::unique_ptr<plugins::PluginTools> tools;
    };
    struct PreparedTools {
        std::shared_ptr<plugins::PluginInstance> instance;
        std::shared_ptr<agent::ToolCatalog> catalog;
        std::unique_ptr<plugins::PluginTools> registration;
    };
    struct TaskReceipt {
        std::uint64_t command = 0;
        std::string digest;
        agent::TurnDiagnostics accepted;
    };
    struct DecisionReceipt {
        agent::ConfirmationReceipt receipt;
        std::uint64_t expected = 0;
        bool approve = false;
    };

    // 针对用户明确选择的包，临时登记活动实例的工具。
    PreparedTools prepare_tools(std::string_view package);
    // 准备并接纳一个工具范围，按入口的授权意图固定人工确认。
    agent::TurnDiagnostics submit_tool(const std::string& tool_id, const std::string& prompt, bool confirm_changes);
    // 撤销不属于当前运行或已失效实例的注册。
    void collect_tool_registration() noexcept;
    // 推进本轮实际证据判断，失败后停止原任务并等待真实退出。
    void advance_verification();

    const agent::ToolClock tool_clock_;
    std::shared_ptr<events::BroadcastChannel> channel_;
    plugins::PluginMailboxHost mailbox_host_;
    const runtime::DomainId dialogue_sender_;
    const events::MailboxId dialogue_mailbox_;
    std::uint64_t dialogue_sequence_ = 0;
    const runtime::DomainId room_sender_;
    const events::MailboxId room_mailbox_;
    plugins::PluginManager plugins_;
    const model::Config model_config_;
    const model::Backend model_backend_;
    const agent::RunVersionLabels labels_;
    std::optional<ToolRegistration> tool_registration_;
    std::shared_ptr<RunLog> run_log_;
    agent::Session session_;
    Conversation conversation_;
    MailboxIntake mailbox_intake_;
    InteractionRoutes interactions_;
    std::shared_ptr<Subagents> subagents_;
    std::unique_ptr<CompanionSession> companion_;
    std::shared_ptr<agent::ToolCatalog> companion_exchange_tools_;
    agent::ConfirmationCaller caller_;
    std::optional<VerificationReport> verification_;
    std::unique_ptr<ToolVerification> verifier_;
    std::uint64_t task_watermark_ = 0;
    std::array<std::optional<TaskReceipt>,16> task_receipts_;
    std::array<DecisionReceipt,64> decisions_{};
    std::size_t next_decision_ = 0;
};
}
