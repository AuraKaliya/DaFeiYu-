#pragma once
#include <dafeiyu/plugin_tool_work.hpp>
#include <dafeiyu/tool_catalog.hpp>

namespace dfy::plugins {
// 一次实例的可选工具注册；描述复制到宿主，空闲注册不持有 DLL 能力表。
class PluginTools {
  public:
    // 完整读取和校验描述后原子注册，旧插件缺少工具能力时保留空集合。
    PluginTools(const std::shared_ptr<PluginInstance>& instance, std::shared_ptr<agent::ToolCatalog> catalog);
    // 撤销本次注册，不终止已经开始的工具或冒充原生实例已退出。
    ~PluginTools();
    // 禁止复制注册撤销责任。
    PluginTools(const PluginTools&) = delete;
    // 禁止赋值或移动覆盖活动注册。
    PluginTools& operator=(const PluginTools&) = delete;
    // 幂等撤销原注册身份，旧运行快照不会转向新实例。
    void revoke() noexcept;
    // 返回全限定工具 ID 副本，不授予执行权限或代码持有。
    std::vector<std::string> ids() const { return ids_; }
  private:
    const std::shared_ptr<agent::ToolCatalog> catalog_;
    std::vector<runtime::DomainId> registrations_;
    std::vector<std::string> ids_;
};
}
