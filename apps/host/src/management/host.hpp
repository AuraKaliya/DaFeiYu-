#pragma once
#include "application.hpp"
#include "dialogue_channel.hpp"
#include "plugin_actions.hpp"
#include "room_location.hpp"
#include "terminal_host.hpp"
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <map>
#include <thread>

namespace dfy::app { class SoftwareLog; }
namespace dfy::app { class MessageStore; }
namespace dfy::app { struct MailboxView; }
namespace dfy::management {
struct PluginDirectory;
enum class StopPhase { requested, conversation, plugins, complete, failed };
struct StopProgress {
    StopPhase phase = StopPhase::requested;
    agent::RunId run{};
    std::chrono::steady_clock::time_point since{};
};
// 业务线程独占应用服务，观察端可独立读取快照或离开。
class Host {
  public:
    using Open = std::function<std::unique_ptr<app::Application>()>;
    using Drive = std::function<void(app::Application&)>;
    // 在业务线程创建应用，按原顺序推进会话与插件。
    explicit Host(Open open, Drive drive = {}, app::SoftwareLog* software = nullptr,
        std::shared_ptr<app::MessageStore> messages = {}, std::string instance = {});
    // 只复制最近发布的快照，锁内不调用应用服务。
    std::optional<app::RunStatusSnapshot> snapshot() const;
    // 复制业务线程已经核实的房间与身体位置，无需唤醒主会话。
    std::optional<RoomLocation> room_location() const;
    // 返回同一份已发布的目录与收件快照，复制期间不调用业务服务。
    std::shared_ptr<const app::MailboxView> mailboxes() const;
    // 借用业务线程最近发布的包目录及实际插件状态。
    std::shared_ptr<const PluginDirectory> packages() const;
    // 由有效管理客户端按连续命令号提交一个后台和目标范围已固定的包操作。
    PluginOperation submit_plugin(const std::string& client, const std::string& instance, PluginCommand command);
    // 读取原后台保留的插件操作回执，支持窗口重新连接。
    PluginOperation plugin_receipt(const std::string& client, const std::string& instance, const std::string& operation) const;
    // 为新观察客户端签发独立命令空间，正常页面刷新沿用原空间。
    std::string open_client();
    // 核对远端观察者身份并更新活跃时间，不读取正文。
    bool visit_client(const std::string& client);
    // 撤销观察客户端的提交身份，已接纳的工作继续由宿主持有。
    void close_client(const std::string& client);
    // 提供仅由原生终端协议使用的命令与输出通道。
    TerminalHost& terminal() noexcept { return terminal_; }
    // 返回本实例在独占目录中打开的消息库，旧内嵌宿主可为空。
    app::MessageStore* messages() const noexcept { return messages_.get(); }
    // 返回绑定当前运行时会话及回合的原实例身份。
    const std::string& instance() const noexcept { return instance_; }
    // 为指定窗口登记或读取其订阅，水位只采样已经写盘的提交。
    std::optional<std::int64_t> message_watermark(const std::string& client, bool subscribe = false) const;
    // 读取共享对话与指定客户端自己的命令水位和回执。
    DialogueView dialogue(std::uint64_t command = 0, const std::string& client = {}) const;
    // 核对命令编号、种类、目标和原话，复用同一命令的回执。
    DialogueReceipt submit_dialogue(DialogueCommand command);
    // 请求业务线程开始有序停止，实际退出由调用方后续查询。
    void close(bool explicit_request = false) noexcept;
    // 在关闭期间复制最近由业务线程核实的等待阶段和原回合。
    StopProgress stop_progress() const;
    // 查询业务线程是否已经离开应用的推进和收尾阶段。
    bool exited() const noexcept { return exited_; }
    // 查询初始化、推进或退出是否出现宿主错误。
    bool failed() const noexcept { return failed_; }
  private:
    TerminalHost terminal_;
    std::shared_ptr<app::MessageStore> messages_;
    std::string instance_;
    struct CommandRecord { DialogueReceipt receipt; DialogueAction action; agent::RunId target; std::uint64_t message; std::string fingerprint; };
    // 用原命令关联的持久号投影最新结果，普通控制命令沿用内存回执。
    DialogueReceipt current_receipt(const CommandRecord& record) const;
    struct Client {
        std::uint64_t latest = 0;
        bool subscribed = false;
        std::chrono::steady_clock::time_point seen = std::chrono::steady_clock::now();
        std::deque<CommandRecord> commands;
        std::uint64_t plugin_latest = 0;
    };
    // 在业务线程执行指定槽位中的命令，并将原回执保留到发布结果。
    void advance_dialogue(app::Application& application, std::optional<DialogueCommand>& slot);
    // 在业务线程复核并执行一个插件操作，同时发布当前目录及原回执。
    void advance_plugin_actions(plugins::PluginManager& manager, std::shared_ptr<const PluginDirectory>& previous);
    // 在宿主锁内以业务采样结算已受理的原代次停止并发布目录。
    void publish_plugin_directory(std::shared_ptr<const PluginDirectory> view);
    // 推进原应用服务、发布值快照并在拥有线程上完成关闭。
    void run(std::stop_token stop, const Open& open, const Drive& drive) noexcept;
    // 在原拥有线程等待会话与插件实际退出，按边界发布收尾进展。
    void shutdown(app::Application& application);
    // 发布新阶段，结算原阶段的耗时与实效。
    void set_stop_phase(StopPhase phase, agent::RunId run = {});

    mutable std::mutex mutex_;
    std::optional<app::RunStatusSnapshot> snapshot_;
    std::optional<RoomLocation> room_location_;
    std::shared_ptr<const app::MailboxView> mailboxes_;
    std::shared_ptr<const PluginDirectory> packages_;
    std::deque<std::shared_ptr<PluginOperation>> plugin_operations_;
    std::size_t queued_plugins_ = 0, waiting_plugins_ = 0;
    std::shared_ptr<const app::ConversationDisplay> display_;
    std::optional<DialogueCommand> pending_, stop_command_;
    mutable std::map<std::string,Client> clients_{{"",{}}};
    bool can_submit_ = false;
    StopProgress progress_;
    app::SoftwareLog* software_;
    std::atomic_bool explicit_stop_ = false;
    std::atomic_bool exited_ = false, failed_ = false;
    std::jthread worker_;
};
}
