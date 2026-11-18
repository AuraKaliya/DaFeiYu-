#pragma once
#include "conversation.hpp"
#include <dafeiyu/plugin_manager.hpp>
#include <dafeiyu/plugin_tools.hpp>

namespace dfy::app {
// 在一个宿主线程上组装会话、插件和共享事件，调用方负责定期推进。
class Application {
  public:
    // 创建使用内置结构化 Provider 的独立应用实例。
    Application(model::Config config, const std::filesystem::path& packages, const std::filesystem::path& user_data);
    // 以显式后端创建独立应用实例，缺少后端由原会话拒绝。
    Application(model::Config config, const std::filesystem::path& packages, const std::filesystem::path& user_data, model::Backend backend);
    // 访问应用拥有的问答与运行服务。
    Conversation& conversation() noexcept { return conversation_; }
    // 在原宿主线程使用应用拥有的插件命令入口。
    plugins::PluginManager& plugins() noexcept { return plugins_; }
    // 显式授予一个已加载实例的只读工具并提交用户目标。
    agent::TurnDiagnostics submit_read_only(const std::string& tool_id, const std::string& prompt);
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

    std::shared_ptr<events::BroadcastChannel> channel_;
    plugins::PluginManager plugins_;
    std::optional<ToolRegistration> tool_registration_;
    agent::Session session_;
    Conversation conversation_;
};
}
