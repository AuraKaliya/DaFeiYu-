#pragma once
#include <dafeiyu/plugin_module_v2.hpp>
#include <dafeiyu/host_services.hpp>
#include <dafeiyu/lifecycle_abi_v2.h>
#include <dafeiyu/hook_abi_v2.h>
#include <string>
#include <vector>
#include <functional>

namespace dfy::plugins {
class PluginInstance;
// 静态能力表与具体实例代次共同绑定，不能把一个实例的能力用于另一个实例。
template<class T> class InstanceCapability {
  public:
    // 禁止复制代码持有和实例绑定。
    InstanceCapability(const InstanceCapability&) = delete;
    // 禁止复制赋值覆盖现有绑定。
    InstanceCapability& operator=(const InstanceCapability&) = delete;
    // 移动转交能力及其代码寿命，来源不能再进入调用。
    InstanceCapability(InstanceCapability&&) = default;
    // 移动替换现有能力，归还旧代码持有。
    InstanceCapability& operator=(InstanceCapability&&) = default;
    // 返回宿主签发的具体实例代次，不暴露原生实例指针。
    runtime::DomainId generation() const noexcept { return generation_; }
  private:
    friend class PluginInstance;
    // 接收当前实例代次与已校验能力，不复制裸函数表到实例作用域外。
    InstanceCapability(runtime::DomainId generation, Capability<T> capability) : generation_(generation), capability_(std::move(capability)) {}
    runtime::DomainId generation_;
    Capability<T> capability_;
};
enum class InstancePhase { loaded, created, active, stopping, destroyed, unloaded, broken };
enum class ShutdownResult { closed, pending, busy, in_use, failed };
struct InstanceLimits { std::size_t references = 32, readers = 1, requests = 4, notifications = 8, code_leases = 16, active_calls = 1; };
struct RetainedInstance { std::string id; InstancePhase phase; uint32_t error; };
// 主宿主线程使用的实例控制器；未完成实例离开局部作用域后仍保留在可重试的有界宿主记录中。
class PluginInstance {
  public:
    static constexpr std::size_t maximum_instances = 64;
    // 装配模块、配置及可选组授权，create 才执行插件实例创建。
    PluginInstance(const Manifest& manifest, const std::filesystem::path& user_data_root,
        std::shared_ptr<events::BroadcastChannel> channel, InstanceLimits limits = {}, std::shared_ptr<events::ConsumerGroup> group = {});
    // 尝试一次非阻塞收尾，未完成则转交宿主保留表，不强制释放原生实例。
    ~PluginInstance();
    // 禁止复制或隐式移动实例控制责任。
    PluginInstance(const PluginInstance&) = delete;
    // 禁止赋值覆盖尚未完成的控制状态。
    PluginInstance& operator=(const PluginInstance&) = delete;
    // 创建一次插件实例，错误通过状态返回，失败清理仍由本控制器承担。
    uint32_t create() noexcept;
    // 启动一次，部分失败先关闭宿主入口，保留实例供停止收尾。
    uint32_t start() noexcept;
    // 在创建开始前以原实例代次装配信箱宿主服务。
    void enable_mailboxes(std::shared_ptr<events::Mailboxes> network, runtime::DomainId app);
    // 在创建前将子会话服务绑定到本实例。
    void enable_agents(std::weak_ptr<PluginAgents> service, const std::shared_ptr<PluginInstance>& instance);
    // 取得当前实例已接入的定向地址，未接入或停止后返回零。
    dfy_v2_mailbox_pair mailbox_addresses() const;
    // 借用启动后固定的 Hook 元数据，只在实例活动期间提供。
    std::span<const dfy_v2_hook_description> hooks() const noexcept;
    // 在原实例的活动保护内调用注册序号对应的 Hook。
    uint32_t decide_hook(std::size_t index, const dfy_v2_hook_input& input);
    // 查询实例启动时协商的可选后台推进能力。
    bool has_activity() const noexcept;
    // 在原实例保护中执行一步，返回下次推进的建议等待时间。
    std::uint32_t advance_activity();
    // 推进一次关停，活动或外部代码持有时不改状态；成功阶段不重放。
    ShutdownResult shutdown() noexcept;
    // 返回当前生命周期阶段副本。
    InstancePhase phase() const noexcept;
    // 返回最近记录的失败码，不保存 DLL 文本或异常对象。
    uint32_t last_error() const noexcept;
    // 返回进程期唯一实例代次，同包重新创建也不复用。
    runtime::DomainId generation() const noexcept;
    // 借用已校验包 ID，实例销毁前保持稳定，不暴露配置内容。
    std::string_view plugin_id() const noexcept;
    // 借用原包清单声明的版本，不把它解释为二进制签名或代码内容摘要。
    std::string_view plugin_version() const noexcept;
    // 从活动实例取得精确版本能力，并保留该实例的代码寿命。
    // 步骤：核对活动阶段并查询能力 → 固定具体实例代次 → 转交代码持有。
    template<class T> std::optional<InstanceCapability<T>> query(std::string_view name, std::uint32_t version) {
        auto capability = active_module().query<T>(name,version);
        if (!capability) return std::nullopt;
        return InstanceCapability<T>(generation(),std::move(*capability));
    }
    // 在宿主服务绑定和代码活动保护内调用本实例能力，回调不得泄露借用表或实例指针。
    // 步骤：拒绝跨实例绑定 → 绑定宿主服务 → 取得代码活动保护 → 调用并返回值结果。
    template<class T, class Action> auto invoke(const InstanceCapability<T>& capability, Action&& action) {
        static_assert(!std::is_reference_v<std::invoke_result_t<Action,const T&,dfy_v2_instance*>>);
        if (capability.generation_ != generation()) throw std::invalid_argument("instance: foreign capability binding");
        auto binding = bind_active(); auto call = capability.capability_.enter();
        return std::invoke(std::forward<Action>(action),call.api(),native_instance());
    }
    // 为显式宿主代码依赖取得凭证，持有期间默认拒绝卸载。
    platform::CodeLease retain_code();
    // 推进读取并安排一项通知，由调用方单独推进共享 FIFO。
    runtime::ScheduleResult advance(runtime::FifoExecutor& executor);
    // 推进读取并向明确所有者的公平调度通路提交一项通知，执行由外层独立推进。
    runtime::ScheduleResult advance(runtime::OwnerScheduler& scheduler, runtime::Handle owner,
        runtime::TaskPriority priority = runtime::TaskPriority::normal);
    // 查询离开控制作用域但尚未安全回收的实例，不包含配置内容。
    static std::vector<RetainedInstance> retained();
    // 在主宿主线程逐项重试保留实例，返回仍未完成的数量。
    static std::size_t retry_retained();
  private:
    // 协商、枚举并验证当前启动的实例声明。
    std::vector<dfy_v2_hook_description> load_hooks();
    // 只向活动实例提供能力查询入口。
    PluginModuleV2& active_module();
    // 核对活动阶段并建立本次原生调用的宿主上下文。
    HostServices::Binding bind_active() const;
    // 在已绑定调用内部借用原生实例，不向外转移实例所有权。
    dfy_v2_instance* native_instance() const noexcept;
    struct State;
    std::unique_ptr<State> state_;
    static State* retained_;
    static std::size_t resident_;
};
}
