#pragma once
#include <dafeiyu/plugin_registry.hpp>
#include <dafeiyu/plugin_dependencies.hpp>
#include <dafeiyu/plugin_package.hpp>

namespace dfy::plugins {
struct LoadedPlugin {
    std::string id;
    InstancePhase phase;
    std::uint32_t error;
    runtime::DomainId generation = 0;
    bool stop_pending = false;
    // 比较本次采样的包、实例代次、生命周期和错误码。
    bool operator==(const LoadedPlugin&) const = default;
};
enum class LifecycleStage { load, stop, unloaded };
struct LifecycleNotice {
    LifecycleStage stage;
    runtime::DomainId instance;
    std::uint32_t load_status = 0;
    ShutdownResult stop_status = ShutdownResult::closed;
    std::chrono::milliseconds wait{};
};
struct ManagedRemoval { ShutdownResult status; RemovalReceipt files; };
struct PluginMailboxHost {
    std::shared_ptr<events::Mailboxes> network;
    runtime::DomainId identity = 0;
};
struct PluginHookSnapshot {
    std::string version = "none";
    std::function<uint32_t(const dfy_v2_hook_input&)> control;
    std::function<std::optional<std::string>(const dfy_v2_text_hook_input&,std::size_t)> rewrite;
    std::function<void(std::uint64_t,std::uint64_t,std::uint32_t)> exited;
};
struct TransitionDecision {
    bool allowed = true;
    unsigned observed = 0, errors = 0;
    std::optional<std::string> value;
    std::string version;
};
// 宿主线程的插件目录及实例管理，不自动加载发现的插件，也不绑定必需插件角色。
class PluginManager {
  public:
    // 固定包目录、独立设置、事件通道、实例上限及可选本体信箱身份，创建时只扫描描述。
    PluginManager(const std::filesystem::path& root, const std::filesystem::path& user_data,
        std::shared_ptr<events::BroadcastChannel> channel, std::size_t capacity = 16, PluginMailboxHost mailboxes = {});
    // 尝试关闭所有实例，未完成责任连同调度器和依赖目录转入有界保留链。
    ~PluginManager();
    // 禁止复制目录、实例与调度所有权。
    PluginManager(const PluginManager&) = delete;
    // 禁止赋值或隐式移动正在使用的管理器。
    PluginManager& operator=(const PluginManager&) = delete;
    // 只刷新磁盘描述，不替换已经运行的实例。
    void refresh();
    // 在管理线程观察后续实例阶段，回调只读通知且不得重入管理器，观察失败与业务结果独立。
    void observe_lifecycle(std::function<void(const LifecycleNotice&)> observer);
    // 在首次加载前把应用子会话服务交给运行时借用。
    void enable_agents(const std::shared_ptr<PluginAgents>& service);
    // 在加载任何实例前登记统一的事件接纳记录器。
    void capture_events(EventCapture capture);
    // 返回独立不可变包目录快照。
    std::shared_ptr<const Catalog> catalog() const;
    // 返回最近一次根目录扫描错误，错误不阻止本体在空插件模式下运行。
    std::string_view directory_error() const noexcept;
    // 返回当前加载或保留中的实例状态副本。
    std::vector<LoadedPlugin> loaded() const;
    // 显式加载并启动指定可用包，失败保留必要清理责任并返回原生命周期状态。
    std::uint32_t load(const std::string& id);
    // 本地安装且不自动加载，拒绝替换运行实例，发布后刷新目录但不改用户配置。
    Manifest install(const std::filesystem::path& source);
    // 默认先卸载再移除包，忙碌或失败时不触碰包；保持用户设置，可重试同 ID 清理残留。
    ManagedRemoval remove(const std::string& id);
    // 按默认拒绝或显式联动规则卸载，忙碌和失败不强制释放模块。
    ShutdownResult unload(const std::string& id, DependencyStop mode = DependencyStop::refuse_dependents);
    // 推进全部实例的就绪通知及正常停止等待，每次只执行一个公平调度步骤。
    runtime::RunResult advance();
    // 从活动实例取一个原生事件，轮转选择发送方。
    std::optional<RoutedEvent> take_routed_event();
    // 按已发布计划执行当前范围的控制 Hook，拒绝优先于确认和允许。
    uint32_t control_hook(const dfy_v2_hook_input& input);
    // 收敛同一字段的修改，冲突或回调错误时拒绝；无修改时返回空可选值。
    std::optional<std::string> rewrite_hook(const dfy_v2_text_hook_input& input, std::size_t limit);
    // 返回本次接纳使用的公开 Hook 登记版本，不包含插件私有设置。
    std::string hook_version();
    // 为一个会话固定 Hook 表及实际参与的原生模块。
    PluginHookSnapshot capture_hooks(runtime::DomainId owner = 0);
    // 执行当前系统边界的已登记处理器，记录只读观察失败。
    TransitionDecision transition_hook(const dfy_v2_transition_hook_input& input, std::size_t limit = 0);
    // 停止新加载并尝试有序关闭全部实例，失败责任仍可显式重试。
    bool close() noexcept;
    // 在调用 close 后查询本轮关闭结果，不用实例历史错误码推断本轮是否失败。
    ShutdownResult close_status() const noexcept;
    // 借用固定的包管理根路径，后续文件操作仍须逐次校验实际边界。
    const std::filesystem::path& root() const;
    // 返回当前具体实例供宿主绑定能力，未知或已回收实例拒绝。
    std::shared_ptr<PluginInstance> instance(const std::string& id) const;
    // 返回运行时依赖目录，不将关系保存为本体配置。
    PluginDependencies& dependencies();
    // 返回失去局部管理器但仍需回收的管理状态数。
    static std::size_t retained() noexcept;
    // 在原宿主线程重试保留状态，只销毁全部完成的管理器。
    static std::size_t retry_retained();
  private:
    struct State;
    std::unique_ptr<State> state_;
    static State* retained_;
};
}
