#pragma once
#include "conversation.hpp"
#include "companion_state.hpp"
#include "companion_messages.hpp"
#include "companion_inbox.hpp"
#include "companion_visit.hpp"
#include "companion_question.hpp"
#include "companion_delivery.hpp"
#include "companion_mining.hpp"
#include "companion_location.hpp"
#include "companion_board.hpp"
#include <dafeiyu/plugin_companion.hpp>
#include <dafeiyu/plugin_manager.hpp>
#include <dafeiyu/plugin_tools.hpp>
#include <array>

namespace dfy::app {
class RunLog;
enum class CompanionMode { scenario, partner };
struct CompanionStatus {
    runtime::DomainId session = 0, instance = 0;
    std::string package, topic;
    bool stopping = false, closed = false;
    std::optional<agent::TurnDiagnostics> turn;
    std::optional<ControlReceipt> control;
    std::uint64_t settled_event = 0;
};
struct CompanionConfirmation {
    std::uint64_t id = 0, revision = 0, order = 0;
    std::string world, player, question, arguments;
    std::string channel, presentation, decision, source;
    bool answered = false;
};

// 在应用宿主线程拥有同一玩家个体的一段游戏会话，工具绑定限原插件实例。
class CompanionSession {
  public:
    // 从活动实例建立独立会话、只读目录及可撤销角色声明。
    CompanionSession(const std::shared_ptr<plugins::PluginInstance>& instance, const model::Config& config,
        const std::shared_ptr<events::BroadcastChannel>& channel, const model::Backend& backend,
        const agent::RunVersionLabels& labels, agent::ToolClock clock, plugins::PluginMailboxHost mailboxes,
        std::shared_ptr<RunLog> log = {});
    // 接纳该会话的只读委托，插件重载时原会话拒绝新动作。
    agent::TurnDiagnostics submit(const std::string& prompt);
    // 推进子会话的结算与实际退出，保留最近一次可信回执。
    void advance();
    // 撤销工具并请求关闭，仍需推进到已退出。
    void close();
    // 停止接纳并向原游戏取得实际释放回执。
    void request_stop();
    // 独立于对话回合，按需向原插件请求真实读回。
    std::uint64_t refresh();
    // 复制已授权状态查询的结果及新旧标记。
    CompanionState state() const { return reader_.state(); }
    // 返回仅从原插件读回签发的游戏消息地址。
    std::optional<CompanionAddress> address() const { return address_; }
    // 显式启用本实例的玩家消息入口。
    void listen(CompanionMode mode = CompanionMode::partner);
    // 查看玩家收件的可信来源、水位和缺口。
    PlayerInboxStatus inbox_status() const { return partner_ ? partner_->input : inbox_.status(); }
    // 读取最近一次玩家查看委托和宿主证据。
    std::optional<CompanionVisit> visit() const { return visit_; }
    // 游戏和管理中心读取同一份问题与发送修订。
    std::optional<CompanionQuestion> question() const { return questions_.current(); }
    // 在可信管理入口按原问题修订授权精确数量。
    CollectionGrant authorize(const CompanionAddress& address, std::uint64_t question, std::uint64_t revision,
        std::uint64_t command, std::uint64_t quantity);
    // 读取已登记的新委托及来源。
    std::optional<CollectionGrant> collection() const { return questions_.grant(); }
    // 区分宿主已授权与原游戏实际接单。
    DeliveryStatus delivery() const { return delivery_.status(); }
    // 读取原生采掘与客户端退出的事实。
    MiningStatus mining() const { return mining_.status(); }
    // 为进度或会合查询选择最近确认的位置或另行重读。
    LocationAnswer locate(LocationPurpose purpose);
    // 返回原探索任务最近一次介入的依据、消息修订和后续验证。
    const std::optional<ExplorationReview>& review() const noexcept { return review_; }
    // 由主 Agent 向当前世界投递委托或询问，重试返回原修订。
    std::uint64_t send(const CompanionAddress& address, CompanionMessageKind kind, std::uint64_t command,
        std::uint64_t reply_to, const std::string& text);
    // 检查当前地址可作为插件信箱入口，场景模式沿原业务入口运行。
    bool accepts_partner_messages(const CompanionAddress& address) const noexcept;
    // 由已固定当前地址的主 Agent 向插件信箱发送一次意图。
    std::uint64_t send_partner(const CompanionAddress& address, CompanionMessageKind kind, const std::string& text);
    // 读取原地址当前委托内最近的消息及实际动作回执。
    std::string partner_receipts(const CompanionAddress& address) const;
    // 读取有界业务消息，供可信界面和主 Agent 使用。
    CompanionDelta messages(std::uint64_t after) const { return messages_.since(after); }
    // 取得指定查看委托的计划板，消息游标沿用本会话的原修订。
    CompanionBoard board(std::optional<std::uint64_t> task, std::uint64_t after) const;
    // 返回无对话正文的状态副本。
    CompanionStatus status() const;
    // 取得原会话身份，调用方用它指定停止或提问的目标。
    runtime::DomainId id() const noexcept { return conversation_.id(); }
    // 判断会话已停止接纳且所有在途工作已退出。
    bool closed() const noexcept { return conversation_.closed() && !reader_.state().pending && !inbox_.busy() && !questions_.busy() &&
        !delivery_.busy() && !mining_.busy() && !control_executor_ && (!partner_ || partner_->closed); }
    // 读取仍保留的原回合结算值副本。
    std::optional<CompletedTurn> result(std::uint64_t turn) const;
    // 取得来自原插件的明确确认事项和已有业务裁定。
    const std::optional<CompanionConfirmation>& confirmation() const noexcept { return confirmation_; }
    // 固定由主 Agent 选中的询问渠道，原问题不接受模型重写。
    std::string route_confirmation(std::uint64_t id, bool game);
    // 按原身份投递可信本体用户的决定，状态等待插件裁定。
    void reply_confirmation(std::uint64_t id, std::uint64_t revision, bool approve);
  private:
    // 检查实例仍活动且属于原注册。
    bool bound() const noexcept;
    // 按插件最新确认的身份绑定或撤销当前消息地址。
    void sync_binding();
    // 向原业务消息交付一次子会话输出或确定失败。
    void finish_message(const CompanionMessage& original, const CompletedTurn* turn);
    // 从原玩家消息接纳单一地标查看。
    void accept_player();
    // 把插件伙伴的业务事实投影到应用计划板。
    void advance_partner();
    // 递交原队首命令；信箱满时保留消息到后续步骤。
    void flush_partner_message();
    // 请求活动插件开始或停止伙伴循环。
    void switch_partner(plugins::PluginInstance& instance, bool listen);
    // 把一件经过原实例信箱交付的事实投影到当前角色。
    void accept_partner(const runtime::StructValue& payload);
    // 把原候选、呈现回执及唯一实际确认映射给主会话。
    void accept_confirmation(std::string_view type, std::string_view data);
    struct PartnerProjection {
        events::MailboxId inbox, outgoing;
        PlayerInboxStatus input;
        std::array<std::pair<std::uint64_t,std::uint64_t>,64> revisions{};
        std::uint64_t settled = 0;
        std::uint64_t delegation = 0, inquiry = 0;
        bool closed = false;
    };
    plugins::PluginMailboxHost mailboxes_;
    std::shared_ptr<RunLog> log_;
    std::optional<PartnerProjection> partner_;
    std::optional<CompanionConfirmation> confirmation_;
    // 启动工具名和参数都已收紧的正式查看回合。
    void start_visit();
    // 在新状态读回上核对探索覆盖并决定是否重读、追问或验证恢复。
    void review_exploration();
    // 从原业务记录复制当前查看及其精确匹配的后续事实。
    BoardTask current_board_task() const;
    // 在收尾期间独立结算同一世界的接管回执。
    void advance_control();
    std::optional<std::string> control_tool_;
    std::optional<ControlReceipt> control_;
    std::unique_ptr<agent::ToolExecutor> control_executor_;
    std::shared_ptr<agent::ToolRequest> control_work_;
    std::weak_ptr<plugins::PluginInstance> instance_;
    std::string package_;
    std::shared_ptr<agent::ToolCatalog> catalog_;
    plugins::PluginTools registration_;
    plugins::CompanionOffer offer_;
    agent::Session session_;
    Conversation conversation_;
    CompanionStateReader reader_;
    std::optional<std::string> survey_tool_;
    std::optional<ExplorationReview> review_;
    std::uint64_t reviewed_request_ = 0;
    CompanionMessages messages_;
    std::optional<CompanionAddress> address_;
    std::string actor_;
    std::optional<CompanionMessage> queued_, delivering_;
    agent::ToolClock clock_;
    CompanionInbox inbox_;
    agent::ToolTime next_refresh_{};
    std::string visit_tool_;
    std::optional<std::string> ask_tool_;
    CompanionQuestions questions_;
    std::string admit_tool_, receipt_tool_;
    CompanionDelivery delivery_;
    std::string mine_tool_, return_tool_;
    CompanionMining mining_;
    std::optional<CompanionVisit> visit_;
    std::array<std::optional<BoardTask>, 8> past_visits_;
    std::size_t next_past_ = 0;
    std::uint64_t discarded_through_ = 0;
    std::shared_ptr<agent::TurnRequest> visit_work_;
    std::optional<CompletedTurn> result_;
    bool stopping_ = false;
};
}
