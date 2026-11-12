#pragma once
#include <dafeiyu/plugin_module_v2.hpp>
#include <dafeiyu/host_services.hpp>
#include <dafeiyu/lifecycle_abi_v2.h>
#include <string>
#include <vector>

namespace dfy::plugins {
enum class InstancePhase { loaded, created, active, stopping, destroyed, unloaded, broken };
enum class ShutdownResult { closed, pending, busy, in_use, failed };
struct InstanceLimits { std::size_t references = 32, readers = 1, requests = 4, notifications = 8, code_leases = 16, active_calls = 1; };
struct RetainedInstance { std::string id; InstancePhase phase; uint32_t error; };
// 主宿主线程使用的实例控制器；未完成实例离开局部作用域后仍保留在可重试的有界宿主记录中。
class PluginInstance {
  public:
    static constexpr std::size_t maximum_instances = 64;
    // 只装配模块、配置及宿主服务，create 才执行插件实例创建。
    PluginInstance(const Manifest& manifest, const std::filesystem::path& user_data_root,
        std::shared_ptr<events::BroadcastChannel> channel, InstanceLimits limits = {});
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
    // 推进一次关停，活动或外部代码持有时不改状态；成功阶段不重放。
    ShutdownResult shutdown() noexcept;
    // 返回当前生命周期阶段副本。
    InstancePhase phase() const noexcept;
    // 返回最近记录的失败码，不保存 DLL 文本或异常对象。
    uint32_t last_error() const noexcept;
    // 为显式宿主代码依赖取得凭证，持有期间默认拒绝卸载。
    platform::CodeLease retain_code();
    // 推进读取并安排一项通知，由调用方单独推进共享 FIFO。
    runtime::ScheduleResult advance(runtime::FifoExecutor& executor);
    // 查询离开控制作用域但尚未安全回收的实例，不包含配置内容。
    static std::vector<RetainedInstance> retained();
    // 在主宿主线程逐项重试保留实例，返回仍未完成的数量。
    static std::size_t retry_retained();
  private:
    struct State;
    std::unique_ptr<State> state_;
    static State* retained_;
    static std::size_t resident_;
};
}
