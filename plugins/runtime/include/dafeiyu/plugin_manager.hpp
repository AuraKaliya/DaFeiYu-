#pragma once
#include <dafeiyu/plugin_registry.hpp>
#include <dafeiyu/plugin_dependencies.hpp>

namespace dfy::plugins {
struct LoadedPlugin { std::string id; InstancePhase phase; std::uint32_t error; };
// 宿主线程的插件目录及实例管理，不自动加载发现的插件，也不绑定必需插件角色。
class PluginManager {
  public:
    // 固定包目录、独立用户数据目录、事件通道和实例上限，创建时只扫描描述。
    PluginManager(const std::filesystem::path& root, const std::filesystem::path& user_data,
        std::shared_ptr<events::BroadcastChannel> channel, std::size_t capacity = 16);
    // 尝试关闭所有实例，未完成责任连同调度器和依赖目录转入有界保留链。
    ~PluginManager();
    // 禁止复制目录、实例与调度所有权。
    PluginManager(const PluginManager&) = delete;
    // 禁止赋值或隐式移动正在使用的管理器。
    PluginManager& operator=(const PluginManager&) = delete;
    // 只刷新磁盘描述，不替换已经运行的实例。
    void refresh();
    // 返回独立不可变包目录快照。
    std::shared_ptr<const Catalog> catalog() const;
    // 返回当前加载或保留中的实例状态副本。
    std::vector<LoadedPlugin> loaded() const;
    // 显式加载并启动指定可用包，失败保留必要清理责任并返回原生命周期状态。
    std::uint32_t load(const std::string& id);
    // 按默认拒绝或显式联动规则卸载，忙碌和失败不强制释放模块。
    ShutdownResult unload(const std::string& id, DependencyStop mode = DependencyStop::refuse_dependents);
    // 推进全部实例的就绪通知及正常停止等待，每次只执行一个公平调度步骤。
    runtime::RunResult advance();
    // 停止新加载并尝试有序关闭全部实例，失败责任仍可显式重试。
    bool close() noexcept;
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
