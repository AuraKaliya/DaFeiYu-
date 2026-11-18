#pragma once
#include "conversation.hpp"
#include <dafeiyu/plugin_manager.hpp>

namespace dfy::app {
// 在一个宿主线程上组装会话、插件和共享事件，调用方负责定期推进。
class Application {
  public:
    // 创建独立应用实例，模型调用可由宿主显式注入。
    Application(model::Config config, const std::filesystem::path& packages, const std::filesystem::path& user_data,
        model::Completion completion = {});
    // 访问应用拥有的问答与运行服务。
    Conversation& conversation() noexcept { return conversation_; }
    // 在原宿主线程使用应用拥有的插件命令入口。
    plugins::PluginManager& plugins() noexcept { return plugins_; }
    // 等原会话实际退出后推进插件关闭，保留管理器的本轮结果。
    plugins::ShutdownResult close_plugins() noexcept;
  private:
    std::shared_ptr<events::BroadcastChannel> channel_;
    plugins::PluginManager plugins_;
    agent::Session session_;
    Conversation conversation_;
};
}
