#pragma once
#include <dafeiyu/host_services.hpp>
#include <dafeiyu/handle_registry.hpp>
#include <thread>
#include <variant>

namespace dfy::plugins::detail {
using HostObject = std::variant<std::shared_ptr<const runtime::Value>>;
struct HostReference { HostObject object; };
struct HostState {
    // 建立本上下文独立引用域并固定宿主线程。
    explicit HostState(std::size_t capacity) : references(capacity), owner(std::this_thread::get_id()) {}
    // 拒绝从其他线程直接操作状态，C 服务由线程绑定进一步隔离。
    void require_owner() const {
        if (owner != std::this_thread::get_id()) throw std::logic_error("host: wrong thread");
    }
    runtime::HandleRegistry<HostReference> references;
    const std::thread::id owner;
};
inline thread_local HostState* current_host = nullptr;
// 显式转换边界句柄，不跨 DLL 传递宿主类或按布局强制重解释。
inline runtime::Handle native(dfy_v2_handle value) noexcept { return {value.domain, value.id, value.generation}; }
// 将宿主引用身份复制为固定 C 表示。
inline dfy_v2_handle abi(runtime::Handle value) noexcept { return {value.domain, value.id, value.generation}; }

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
    catch (const std::bad_variant_access&) { return DFY_V2_INVALID_ARGUMENT; }
    catch (...) { return DFY_V2_FAILED; }
    return DFY_V2_FAILED;
}
}
