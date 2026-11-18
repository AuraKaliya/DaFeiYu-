#pragma once
#include "conversation.hpp"
#include "companion_state.hpp"
#include "companion_messages.hpp"
#include "companion_inbox.hpp"
#include "companion_visit.hpp"
#include "companion_question.hpp"
#include "companion_delivery.hpp"
#include "companion_mining.hpp"
#include <dafeiyu/plugin_companion.hpp>
#include <dafeiyu/plugin_tools.hpp>

namespace dfy::app {
struct CompanionStatus {
    runtime::DomainId session = 0, instance = 0;
    std::string package, topic;
    bool stopping = false, closed = false;
    std::optional<agent::TurnDiagnostics> turn;
};

// 在应用宿主线程拥有同一玩家个体的一段游戏会话，工具绑定限原插件实例。
class CompanionSession {
  public:
    // 从活动实例建立独立会话、只读目录及可撤销角色声明。
    CompanionSession(const std::shared_ptr<plugins::PluginInstance>& instance, const model::Config& config,
        const std::shared_ptr<events::BroadcastChannel>& channel, const model::Backend& backend,
        const agent::RunVersionLabels& labels, agent::ToolClock clock);
    // 接纳该会话的只读委托，插件重载时原会话拒绝新动作。
    agent::TurnDiagnostics submit(const std::string& prompt);
    // 推进子会话的结算与实际退出，保留最近一次可信回执。
    void advance();
    // 撤销工具并请求关闭，仍需推进到已退出。
    void close();
    // 独立于对话回合，按需向原插件请求真实读回。
    std::uint64_t refresh();
    // 复制已授权状态查询的结果及新旧标记。
    CompanionState state() const { return reader_.state(); }
    // 返回仅从原插件读回签发的游戏消息地址。
    std::optional<CompanionAddress> address() const { return address_; }
    // 显式启用本实例的玩家消息入口。
    void listen();
    // 查看玩家收件的可信来源、水位和缺口。
    PlayerInboxStatus inbox_status() const { return inbox_.status(); }
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
    // 由主 Agent 向当前世界投递委托或询问，重试返回原修订。
    std::uint64_t send(const CompanionAddress& address, CompanionMessageKind kind, std::uint64_t command,
        std::uint64_t reply_to, const std::string& text);
    // 读取有界业务消息，供可信界面和主 Agent 使用。
    CompanionDelta messages(std::uint64_t after) const { return messages_.since(after); }
    // 返回无对话正文的状态副本。
    CompanionStatus status() const;
    // 取得原会话身份，调用方用它指定停止或提问的目标。
    runtime::DomainId id() const noexcept { return conversation_.id(); }
    // 判断会话已停止接纳且所有在途工作已退出。
    bool closed() const noexcept { return conversation_.closed() && !reader_.state().pending && !inbox_.busy() && !questions_.busy() &&
        !delivery_.busy() && !mining_.busy(); }
    // 读取仍保留的原回合结算值副本。
    std::optional<CompletedTurn> result(std::uint64_t turn) const;
  private:
    // 检查实例仍活动且属于原注册。
    bool bound() const noexcept;
    // 按插件最新确认的身份绑定或撤销当前消息地址。
    void sync_binding();
    // 向原业务消息交付一次子会话输出或确定失败。
    void finish_message(const CompanionMessage& original, const CompletedTurn* turn);
    // 从原玩家消息接纳单一地标查看。
    void accept_player();
    // 启动工具名和参数都已收紧的正式查看回合。
    void start_visit();
    std::weak_ptr<plugins::PluginInstance> instance_;
    std::string package_;
    std::shared_ptr<agent::ToolCatalog> catalog_;
    plugins::PluginTools registration_;
    plugins::CompanionOffer offer_;
    agent::Session session_;
    Conversation conversation_;
    CompanionStateReader reader_;
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
    std::shared_ptr<agent::TurnRequest> visit_work_;
    std::optional<CompletedTurn> result_;
    bool stopping_ = false;
};
}
