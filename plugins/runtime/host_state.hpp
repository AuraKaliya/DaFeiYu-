#pragma once
#include <dafeiyu/host_services.hpp>
#include <dafeiyu/handle_registry.hpp>
#include <dafeiyu/data_abi_v2.h>
#include <dafeiyu/event_abi_v2.h>
#include <dafeiyu/resource_owner.hpp>
#include <dafeiyu/enum_descriptor.hpp>
#include <dafeiyu/union_descriptor.hpp>
#include <thread>
#include <variant>

namespace dfy::plugins::detail {
struct NotificationReference { runtime::Handle registration; };
using HostObject = std::variant<std::shared_ptr<const runtime::Value>, std::shared_ptr<const runtime::StructDescriptor>,
    std::shared_ptr<const runtime::StructValue>, std::shared_ptr<events::SubscriptionReader>, std::shared_ptr<events::ReadRequest>,
    std::shared_ptr<NotificationReference>, std::shared_ptr<const runtime::EnumDescriptor>, std::shared_ptr<const runtime::FieldDescriptor>,
    std::shared_ptr<const runtime::UnionDescriptor>>;
struct HostReference { HostObject object; };
struct ReaderSlot {
    std::shared_ptr<events::SubscriptionReader> reader;
    std::shared_ptr<events::SubscriptionQueue> queue;
    runtime::Handle resource;
};
struct HostState : std::enable_shared_from_this<HostState> {
    // 建立本上下文独立引用域并固定宿主线程。
    HostState(std::size_t capacity, std::size_t reader_capacity)
        : references(capacity), owner(std::this_thread::get_id()), readers(reader_capacity), resources(capacity) {}
    // 拒绝从其他线程直接操作状态，C 服务由线程绑定进一步隔离。
    void require_owner() const {
        if (owner != std::this_thread::get_id()) throw std::logic_error("host: wrong thread");
    }
    runtime::HandleRegistry<HostReference> references;
    const std::thread::id owner;
    std::vector<ReaderSlot> readers;
    std::shared_ptr<events::BroadcastChannel> channel;
    std::size_t request_capacity = 0;
    runtime::ResourceOwner resources;
    platform::CodeLease code;
    std::unique_ptr<runtime::CompletionNotifications> notifications;
    bool cleanup_failed = false;
    std::size_t callback_pins = 0;
};
inline thread_local HostState* current_host = nullptr;
// 显式转换边界句柄，不跨 DLL 传递宿主类或按布局强制重解释。
inline runtime::Handle native(dfy_v2_handle value) noexcept { return {value.domain, value.id, value.generation}; }
// 将宿主引用身份复制为固定 C 表示。
inline dfy_v2_handle abi(runtime::Handle value) noexcept { return {value.domain, value.id, value.generation}; }
// 为已确认非空的宿主对象建立独立引用，异常由调用边界处理。
inline dfy_v2_handle store_object(HostState& host, HostObject object) {
    return abi(host.references.insert(std::make_shared<HostReference>(HostReference{std::move(object)})));
}
// 从已验证引用借用精确种类的宿主数据，不延长来源引用。
template<class T>
const T& object(HostState& host, dfy_v2_handle source) {
    return *std::get<std::shared_ptr<const T>>(host.references.get(native(source)).object);
}
// 按精确数据版本将已实现的宿主值种类转换为 C 分类编号。
uint32_t value_kind(runtime::ValueKind kind, uint32_t profile = 1);
// 返回只读静态类型服务，实际访问仍需要宿主绑定。
const dfy_v2_type_api& type_api();
// 返回只读基础值服务表。
const dfy_v2_value_api& value_api();
// 返回只读结构载荷服务表。
const dfy_v2_struct_api& struct_api();
// 返回版本 2 的嵌套类型访问能力。
const dfy_v2_type_api_v2& type_api_v2();
// 返回版本 2 的值分类及结构访问能力。
const dfy_v2_value_api_v2& value_api_v2();
// 返回版本 2 的通用结构字段引用能力。
const dfy_v2_struct_api_v2& struct_api_v2();
// 返回实际实现的标量数组访问能力。
const dfy_v2_array_api& array_api();
// 返回实际实现的字符串键标量映射访问能力。
const dfy_v2_map_api& map_api();
// 返回闭合枚举定义与成员选择的反射服务。
const dfy_v2_enum_api& enum_api();
// 返回独立字段约束的反射和命名描述访问服务。
const dfy_v2_constraint_api& constraint_api();
// 返回版本 2 的完整容器元素约束反射。
const dfy_v2_constraint_api_v2& constraint_api_v2();
// 返回带标签联合的反射与实际内容访问服务。
const dfy_v2_union_api& union_api();
// 返回类型化读取服务。
const dfy_v2_reader_api& reader_api();
// 返回生产操作状态服务，当前由完整读取请求支撑。
const dfy_v2_operation_state_api& operation_api();
// 返回生产读取的协作取消服务。
const dfy_v2_operation_cancel_api& cancel_api();
// 返回绑定代码寿命的 C 通知服务。
const dfy_v2_operation_notify_api& notification_api();
// 返回独立订阅的接纳与关闭服务。
const dfy_v2_subscription_api& subscription_api();
// 在交付队列已结束后解除目录与资源责任，保留外部数据引用。
void finish_reader(HostState& host, const std::shared_ptr<events::SubscriptionReader>& reader);
// 从当前域取得完整读取请求，不能只保留操作状态视图。
inline std::shared_ptr<events::ReadRequest> request(HostState& host, dfy_v2_handle value) {
    return std::get<std::shared_ptr<events::ReadRequest>>(host.references.get(native(value)).object);
}

// 捕获宿主异常并统一转换调用状态，错误缓冲不回显内部文本。
// 步骤：清空错误 → 检查调用上下文 → 执行能力逻辑 → 按错误类别映射。
template<class Action>
dfy_v2_status boundary(dfy_v2_error* error, Action action) noexcept {
    if (error) error->length = 0;
    if (!current_host) return DFY_V2_INVALID_CONTEXT;
    try { return action(*current_host); }
    catch (const runtime::ReferenceError& failure) {
        switch (failure.reason()) {
        case runtime::ReferenceFailure::invalid_handle: return DFY_V2_INVALID_HANDLE;
        case runtime::ReferenceFailure::not_shareable: return DFY_V2_UNSUPPORTED;
        case runtime::ReferenceFailure::exhausted: return DFY_V2_RESOURCE_EXHAUSTED;
        }
    }
    catch (const std::bad_alloc&) { return DFY_V2_RESOURCE_EXHAUSTED; }
    catch (const std::length_error&) { return DFY_V2_RESOURCE_EXHAUSTED; }
    catch (const std::invalid_argument&) { return DFY_V2_INVALID_ARGUMENT; }
    catch (const std::domain_error&) { return DFY_V2_UNSUPPORTED; }
    catch (const std::bad_variant_access&) { return DFY_V2_INVALID_ARGUMENT; }
    catch (...) { return DFY_V2_FAILED; }
    return DFY_V2_FAILED;
}
}
