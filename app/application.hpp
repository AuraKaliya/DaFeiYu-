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
    // 提交有明确工具范围的用户目标，状态变更进入原会话的人工确认。
    agent::TurnDiagnostics submit_confirmed(const std::string& tool_id, const std::string& prompt);
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

    // 准备并接纳一个工具范围，按入口的授权意图固定人工确认。
    agent::TurnDiagnostics submit_tool(const std::string& tool_id, const std::string& prompt, bool confirm_changes);
    // 撤销不属于当前运行或已失效实例的注册。
    void collect_tool_registration() noexcept;

    std::shared_ptr<events::BroadcastChannel> channel_;
    plugins::PluginManager plugins_;
    std::optional<ToolRegistration> tool_registration_;
    agent::Session session_;
    Conversation conversation_;
    agent::ConfirmationCaller caller_;
};
}
