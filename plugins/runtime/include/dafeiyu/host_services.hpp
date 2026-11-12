#pragma once
#include <dafeiyu/value.hpp>
#include <dafeiyu/struct_descriptor.hpp>
#include <dafeiyu/struct_value.hpp>
#include <dafeiyu/plugin_abi_v2.h>
#include <memory>

namespace dfy::plugins {
namespace detail { struct HostState; }
// 宿主线程内的服务上下文；通过作用域绑定调用方，不向 DLL 暴露 C++ 对象。
class HostServices {
  public:
    class Binding {
      public:
        // 返回到先前绑定，必须按同线程后进先出结束，宿主状态覆盖当前调用。
        ~Binding();
        // 禁止复制调用作用域。
        Binding(const Binding&) = delete;
        // 禁止赋值或移动，保持绑定栈顺序。
        Binding& operator=(const Binding&) = delete;
      private:
        friend class HostServices;
        // 核对线程后绑定上下文，并保持本次调用所需的宿主状态。
        explicit Binding(std::shared_ptr<detail::HostState> state);
        std::shared_ptr<detail::HostState> state_;
        detail::HostState* previous_;
    };
    // 创建独立有界引用域；创建、操作及销毁均由同一宿主线程负责。
    explicit HostServices(std::size_t reference_capacity);
    // 释放控制方持有；外部引用须在最后宿主状态退出前结束。
    ~HostServices();
    // 禁止复制服务身份。
    HostServices(const HostServices&) = delete;
    // 禁止赋值或隐式移动服务控制器。
    HostServices& operator=(const HostServices&) = delete;
    // 为当前原生调用绑定服务上下文，嵌套调用返回后恢复外层。
    Binding bind() const;
    // 向当前实例暴露宿主不可变值，返回独立持有引用。
    dfy_v2_handle expose(std::shared_ptr<const runtime::Value> value, bool shareable = true);
    // 向当前实例暴露宿主结构描述，字段视图依赖此持有引用。
    dfy_v2_handle expose(std::shared_ptr<const runtime::StructDescriptor> descriptor);
    // 向当前实例暴露不可变结构载荷，不复制为供应商或插件布局。
    dfy_v2_handle expose(std::shared_ptr<const runtime::StructValue> value);
    // 返回本上下文当前持有引用数，不计其他宿主上下文。
    std::size_t references() const;
    // 在有效绑定中查询已实现宿主能力，失败清空输出且异常不跨 C ABI。
    static dfy_v2_status DFY_V2_CALL query(dfy_v2_string_view name, uint32_t version,
        const void** output, dfy_v2_error* error) noexcept;
  private:
    std::shared_ptr<detail::HostState> state_;
};
}
