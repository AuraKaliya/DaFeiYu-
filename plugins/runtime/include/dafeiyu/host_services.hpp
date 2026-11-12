#pragma once
#include <dafeiyu/value.hpp>
#include <dafeiyu/struct_descriptor.hpp>
#include <dafeiyu/struct_value.hpp>
#include <dafeiyu/subscription_reader.hpp>
#include <dafeiyu/broadcast_channel.hpp>
#include <dafeiyu/completion_notifications.hpp>
#include <dafeiyu/code_module.hpp>
#include <dafeiyu/plugin_abi_v2.h>
#include <memory>

namespace dfy::plugins {
namespace detail { struct HostState; struct CallbackSupport; }
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
        friend struct detail::CallbackSupport;
        // 核对线程后绑定上下文，并保持本次调用所需的宿主状态。
        explicit Binding(std::shared_ptr<detail::HostState> state);
        std::shared_ptr<detail::HostState> state_;
        detail::HostState* previous_;
    };
    // 创建独立有界引用域；创建、操作及销毁均由同一宿主线程负责。
    explicit HostServices(std::size_t reference_capacity, std::size_t reader_capacity = 1);
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
    // 登记并独立保持预创建读取控制器，返回不可共享引用；客户端释放不结束控制器。
    dfy_v2_handle expose(std::shared_ptr<events::SubscriptionReader> reader);
    // 在数据或取消变化后推进所有本实例读取，并回收已无客户端的终结结果。
    void advance();
    // 为本实例启用订阅服务，明确每个读取控制器的请求容量，禁止替换活动通道。
    void enable_events(std::shared_ptr<events::BroadcastChannel> channel, std::size_t request_capacity);
    // 为本实例启用有界 C 通知，代码凭证须属于该插件，禁止重复配置。
    void enable_notifications(platform::CodeLease code, std::size_t capacity);
    // 将一项就绪通知排入宿主 FIFO，不内联调用 C 消费者。
    runtime::ScheduleResult schedule_one(runtime::FifoExecutor& executor);
    // 返回仍需清理的宿主登记数量，普通数据引用不计入。
    std::size_t resources() const;
    // 返回已记录的清理契约失败，不能据此强制释放实例。
    bool cleanup_failed() const;
    // 活动通知时拒绝且不改变接纳；否则停止新工作、撤销通知并取消关闭所有订阅。
    bool begin_shutdown();
    // 插件销毁后完成已开始的收尾，释放剩余引用及代码；未静止或有清理错误时拒绝。
    bool finish_shutdown();
    // 返回本服务持有的代码凭证数，供外层区分自身与外部代码依赖。
    std::size_t code_holds() const;
    // 返回本上下文当前持有引用数，不计其他宿主上下文。
    std::size_t references() const;
    // 在有效绑定中查询已实现宿主能力，失败清空输出且异常不跨 C ABI。
    static dfy_v2_status DFY_V2_CALL query(dfy_v2_string_view name, uint32_t version,
        const void** output, dfy_v2_error* error) noexcept;
  private:
    std::shared_ptr<detail::HostState> state_;
};
}
