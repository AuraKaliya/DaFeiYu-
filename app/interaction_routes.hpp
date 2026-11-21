#pragma once
#include "ask.hpp"
#include <dafeiyu/plugin_manager.hpp>

namespace dfy::app {
class RunLog;

// 将插件事件连接到通用提问服务及原插件地址。
class InteractionRoutes {
  public:
    // 使用应用现有的插件、信箱网络和 Trace。
    InteractionRoutes(plugins::PluginManager& plugins, plugins::PluginMailboxHost mail, agent::ToolClock clock,
        std::shared_ptr<RunLog> log);
    // 消费一个路由事件，并推进在途呈现与结果投递。
    void advance();
  private:
    struct Exchange {
        plugins::RoutedEvent origin;
        std::uint64_t item, revision, ask = 0;
        bool sent = false;
    };
    // 核对原插件及其当前接收地址。
    events::MailboxId destination(const plugins::RoutedEvent& event) const;
    // 接收原实例解出的提问或渠道反馈。
    void accept(plugins::RoutedEvent event);
    // 尝试投递一份呈现请求或最终结果。
    bool advance_exchange(Exchange& exchange);

    plugins::PluginManager& plugins_;
    plugins::PluginMailboxHost mail_;
    agent::ToolClock clock_;
    std::shared_ptr<RunLog> log_;
    AskService asks_;
    std::vector<Exchange> exchanges_;
};
}
