#pragma once
#include <dafeiyu/plugin_manifest.hpp>
#include <dafeiyu/code_module.hpp>
#include <dafeiyu/plugin_abi_v2.h>

namespace dfy::plugins {
// 独立的 v2 模块控制器；复用代码寿命保护，不隐式改变 v1 或创建实例。
class PluginModuleV2 {
  public:
    // 复核清单并在活动保护内查询精确 v2 引导表，失败回收本次模块引用。
    PluginModuleV2(const Manifest& manifest, std::size_t lease_capacity, std::size_t call_capacity = 1);
    // 默认释放控制方持有，独立代码凭证仍保持模块寿命。
    ~PluginModuleV2() = default;
    // 禁止复制模块控制责任。
    PluginModuleV2(const PluginModuleV2&) = delete;
    // 禁止赋值或隐式移动，保持控制器及其借用关系稳定。
    PluginModuleV2& operator=(const PluginModuleV2&) = delete;
    // 查询本控制器是否仍持有已校验的引导表。
    bool open() const noexcept { return api_.query_interface != nullptr; }
    // 尝试关闭本次模块引用，忙碌或仍有代码凭证时保持接口，释放失败可重试。
    platform::ModuleClose close();
  private:
    platform::CodeModule module_;
    dfy_v2_plugin_api api_{};
};
}
