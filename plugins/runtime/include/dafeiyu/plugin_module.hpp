#pragma once
#include <dafeiyu/plugin_manifest.hpp>
#include <dafeiyu/dynamic_library.hpp>
#include <dafeiyu/plugin_abi.h>

namespace dfy::plugins {
// 单线程持有已校验的接口和一次 DLL 引用，不创建或停止插件实例。
class PluginModule {
  public:
    // 复核清单后加载并查询 ABI；失败自动回收本次取得的模块引用。
    explicit PluginModule(const Manifest& manifest);
    // 默认析构由平台成员尽力释放引用，调用方须先结束所有借用与调用。
    ~PluginModule() = default;
    // 禁止复制和隐式移动，保持接口与模块引用的归属稳定。
    PluginModule(const PluginModule&) = delete;
    // 禁止赋值，避免覆盖仍被借用的接口。
    PluginModule& operator=(const PluginModule&) = delete;

    // 返回宿主保存的只读函数表，引用和函数地址仅在模块持有期间有效。
    const dfy_plugin_api& api() const;
    // 释放本次模块引用并使接口失效；失败保留状态供重试，不处理实例生命周期。
    void close();

  private:
    platform::DynamicLibrary library_;
    dfy_plugin_api api_{};
};
}
