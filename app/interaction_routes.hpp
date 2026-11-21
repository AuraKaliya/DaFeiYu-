#pragma once
#include "ask.hpp"
#include "mailbox_intake.hpp"
#include <dafeiyu/plugin_manager.hpp>

namespace dfy::app {
class RunLog;

// 将插件事件连接到通用提问服务及原插件地址。
class InteractionRoutes {
  public:
    // 使用应用现有的插件、信箱网络和 Trace。
    InteractionRoutes(plugins::PluginManager& plugins, plugins::PluginMailboxHost mail, MailboxIntake& intake,
        agent::ToolClock clock, std::shared_ptr<RunLog> log, bool auto_wake);
    // 消费一个路由事件，并推进在途呈现与结果投递。
    void advance();
    // 对原终端提问取一份展示副本。
    std::optional<std::pair<std::uint64_t,AskSpec>> console_question() const;
    // 记录原终端渠道已显示或提交原真人答复。
    bool console_answer(std::uint64_t id, std::optional<std::string> answer);
    // 暂停或恢复已登记插件地址上的自动唤醒。
    void set_auto_wake(bool enabled) noexcept { auto_wake_ = enabled; }
  private:
    struct Exchange {
        plugins::RoutedEvent origin;
        std::uint64_t item, revision, ask = 0;
        agent::ToolTime deadline;
        events::MailboxId main_box{};
        bool initial_queued = false, sent = false, result_queued = false, reply_requested = false;
    };
    // 核对原插件及其当前接收地址。
    events::MailboxId destination(const plugins::RoutedEvent& event) const;
    // 接收原实例解出的提问或渠道反馈。
    void accept(plugins::RoutedEvent event);
    // 尝试投递一份呈现请求或最终结果。
    bool advance_exchange(Exchange& exchange);
    // 按事件来源及原身份定位仍在等待的事项。
    Exchange* find(events::EventId event);
    // 向指定受信信箱投递原事项上下文。
    events::DeliveryReceipt post(runtime::DomainId sender, events::MailboxId box, std::string_view category, std::uint32_t version,
        std::string_view payload);
    // 读取或创建原插件登记的主 Agent 信箱。
    events::MailboxId main_box(const plugins::RoutedEvent& origin);
    // 给当前主回合装配仅绑定原信的提问或回信工具。
    void configure_main(const MailboxDelivery& delivery, agent::RunOptions& options);
    // 开始同一事项的一次 ASK，可供原生和主 Agent 共用。
    void open_ask(Exchange& exchange, std::string channel);
    // 读取来自信件的可信事项标记。
    Exchange* original(const MailboxDelivery& delivery);

    plugins::PluginManager& plugins_;
    plugins::PluginMailboxHost mail_;
    MailboxIntake& intake_;
    agent::ToolClock clock_;
    std::shared_ptr<RunLog> log_;
    AskService asks_;
    std::vector<Exchange> exchanges_;
    std::map<runtime::DomainId,events::MailboxId> main_boxes_;
    std::shared_ptr<agent::ToolCatalog> main_run_tools_;
    bool auto_wake_;
};
}
