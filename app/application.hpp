#pragma once
#include "conversation.hpp"
#include "tool_verification.hpp"
#include <dafeiyu/plugin_manager.hpp>
#include <dafeiyu/plugin_tools.hpp>
#include <array>

namespace dfy::app {
struct VerificationReport {
    agent::RunId run;
    std::optional<agent::ObservationMatch> match;
};
struct TaskAdmission {
    agent::TurnDiagnostics status;
    bool reused = false;
};

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
    // 提交同一实例的无参读取及变更工具，由宿主比对变更后的实际文本状态。
    agent::TurnDiagnostics submit_verified(const std::string& reader, const std::string& change, const std::string& expected,
        const std::string& prompt);
    // 使用当前宿主的顺序请求号提交任务，同一已接纳命令复用原回执。
    TaskAdmission submit_verified_once(std::uint64_t command, const std::string& reader, const std::string& change,
        const std::string& expected, const std::string& prompt);
    // 只读查询仍在保留范围的请求及原运行身份。
    std::optional<agent::RunId> task_run(std::uint64_t command) const noexcept;
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

    std::shared_ptr<events::BroadcastChannel> channel_;
    plugins::PluginManager plugins_;
    std::optional<ToolRegistration> tool_registration_;
    agent::Session session_;
    Conversation conversation_;
    agent::ConfirmationCaller caller_;
    std::optional<VerificationReport> verification_;
    std::unique_ptr<ToolVerification> verifier_;
    std::uint64_t task_watermark_ = 0;
    std::array<std::optional<TaskReceipt>,16> task_receipts_;
    std::array<DecisionReceipt,64> decisions_{};
    std::size_t next_decision_ = 0;
};
}
