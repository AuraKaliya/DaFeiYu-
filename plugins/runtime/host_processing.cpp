#include "host_state.hpp"

namespace dfy::plugins {
// 在副作用前建立完整处理队列，再登记宿主停止责任。
void HostServices::enable_processing(std::size_t capacity, bool ordered, std::shared_ptr<runtime::Budget> owner_budget) {
    state_->require_owner();
    if (state_->processing) throw std::logic_error("host: processing already enabled");
    auto queue = std::make_unique<events::ProcessingQueue>(capacity,ordered,std::move(owner_budget));
    const auto resource = state_->resources.prepare(); state_->resources.activate(resource);
    state_->processing = std::move(queue); state_->processing_resource = resource;
}
// 先预留 C 引用，确保处理接纳后不会因边界分配失败而丢失控制句柄。
// 步骤：检查开放处理能力 → 预留不可共享引用 → 接纳并转交处理记录或回滚。
dfy_v2_handle HostServices::submit_processing(const events::EventLease& event) {
    state_->require_owner();
    if (!state_->processing || state_->resources.state() != runtime::OwnerState::open) throw std::logic_error("host: processing unavailable");
    auto reference = std::make_shared<detail::HostReference>(detail::HostReference{std::shared_ptr<events::ProcessingRecord>{}});
    const auto reserved = state_->references.insert(reference,false);
    try { reference->object = state_->processing->submit(event); }
    catch (...) { state_->references.release(reserved); throw; }
    return detail::abi(reserved);
}
// 在卸载改变任何接纳状态前检查运行中处理。
bool HostServices::processing_busy() const { state_->require_owner(); return state_->processing && state_->processing->running(); }
namespace detail {
namespace {
// 在当前绑定的引用域读取完整处理记录，错误种类和错误实例均拒绝。
std::shared_ptr<events::ProcessingRecord> work(HostState& host, dfy_v2_handle handle) {
    return std::get<std::shared_ptr<events::ProcessingRecord>>(host.references.get(native(handle)).object);
}
// 显式映射宿主业务及退出状态，不依赖内部枚举数值。
uint32_t operation_state(runtime::OperationState state) {
    switch (state) {
    case runtime::OperationState::pending: return DFY_V2_OPERATION_PENDING;
    case runtime::OperationState::succeeded: return DFY_V2_OPERATION_SUCCEEDED;
    case runtime::OperationState::failed: return DFY_V2_OPERATION_FAILED;
    case runtime::OperationState::cancelled: return DFY_V2_OPERATION_CANCELLED;
    }
    throw std::logic_error("host: invalid operation state");
}
// 返回独立快照，不触发处理开始、取消裁定或退出。
// 步骤：清空输出 → 核对处理引用 → 映射阶段和状态 → 复制事件与顺序身份。
dfy_v2_status DFY_V2_CALL read(dfy_v2_handle handle, dfy_v2_processing_info* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error,[&](auto& host) {
        const auto record = work(host,handle); dfy_v2_processing_info info{};
        switch (record->phase()) {
        case events::ProcessingPhase::waiting: info.phase = DFY_V2_PROCESSING_WAITING; break;
        case events::ProcessingPhase::running: info.phase = DFY_V2_PROCESSING_RUNNING; break;
        case events::ProcessingPhase::exited: info.phase = DFY_V2_PROCESSING_EXITED; break;
        }
        info.result = operation_state(record->completion()->state()); info.exit = operation_state(record->exit()->state());
        switch (record->exit()->cancellation_reason()) {
        case runtime::CancelReason::none: break;
        case runtime::CancelReason::user: info.cancel_reason = DFY_V2_CANCEL_USER; break;
        case runtime::CancelReason::owner_stop: info.cancel_reason = DFY_V2_CANCEL_OWNER_STOP; break;
        case runtime::CancelReason::deadline: info.cancel_reason = DFY_V2_CANCEL_DEADLINE; break;
        }
        info.event_domain = record->event().id.domain; info.event_sequence = record->event().id.sequence;
        if (const auto order = record->ordering()) { info.order_domain = order->domain(); info.order_sequence = order->sequence(); }
        *output = info; return DFY_V2_OK;
    });
}
// 只确认已经具备资格的处理开始，取消或旧终态不会重新执行。
dfy_v2_status DFY_V2_CALL start(dfy_v2_handle handle, dfy_v2_error* error) {
    return boundary(error,[&](auto& host) {
        if (!host.processing) return DFY_V2_UNSUPPORTED;
        const auto result = host.processing->start(work(host,handle));
        return result == events::StartResult::started ? DFY_V2_OK : result == events::StartResult::waiting ? DFY_V2_BUSY : DFY_V2_FAILED;
    });
}
// 校验并固定业务结果，不在成功时隐式归还占用。
dfy_v2_status DFY_V2_CALL complete(dfy_v2_handle handle, uint32_t terminal, dfy_v2_error* error) {
    return boundary(error,[&](auto& host) {
        if (!host.processing) return DFY_V2_UNSUPPORTED;
        runtime::OperationState result;
        switch (terminal) {
        case DFY_V2_OPERATION_SUCCEEDED: result = runtime::OperationState::succeeded; break;
        case DFY_V2_OPERATION_FAILED: result = runtime::OperationState::failed; break;
        case DFY_V2_OPERATION_CANCELLED: result = runtime::OperationState::cancelled; break;
        default: return DFY_V2_INVALID_ARGUMENT;
        }
        host.processing->complete(work(host,handle),result); return DFY_V2_OK;
    });
}
// 消费者明确确认实际退出后才归还顺序占用。
dfy_v2_status DFY_V2_CALL finish(dfy_v2_handle handle, dfy_v2_error* error) {
    return boundary(error,[&](auto& host) {
        if (!host.processing) return DFY_V2_UNSUPPORTED;
        host.processing->finish(work(host,handle)); return DFY_V2_OK;
    });
}
// 登记协作取消并返回首因回执，不自动完成运行中的业务。
// 步骤：清空回执 → 校验取消原因 → 登记取消并显式转换回执。
dfy_v2_status DFY_V2_CALL cancel(dfy_v2_handle handle, uint32_t reason, uint32_t* output, dfy_v2_error* error) {
    if (output) *output = 0;
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error,[&](auto& host) {
        runtime::CancelReason requested;
        switch (reason) {
        case DFY_V2_CANCEL_USER: requested = runtime::CancelReason::user; break;
        case DFY_V2_CANCEL_OWNER_STOP: requested = runtime::CancelReason::owner_stop; break;
        case DFY_V2_CANCEL_DEADLINE: requested = runtime::CancelReason::deadline; break;
        default: return DFY_V2_INVALID_ARGUMENT;
        }
        switch (work(host,handle)->request_cancel(requested)) {
        case runtime::CancelDisposition::recorded: *output = DFY_V2_CANCEL_RECORDED; break;
        case runtime::CancelDisposition::already_requested: *output = DFY_V2_CANCEL_ALREADY_REQUESTED; break;
        case runtime::CancelDisposition::already_settled: *output = DFY_V2_CANCEL_ALREADY_SETTLED; break;
        }
        return DFY_V2_OK;
    });
}
// 以处理记录为别名所有者，保留载荷也继续占处理结果预算。
dfy_v2_status DFY_V2_CALL payload(dfy_v2_handle handle, dfy_v2_handle* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error,[&](auto& host) {
        const auto record = work(host,handle);
        *output = store_object(host,std::shared_ptr<const runtime::StructValue>(record,record->event().payload.get())); return DFY_V2_OK;
    });
}
}
// 返回精确版本的处理控制表，不改变既有读取与操作表布局。
const dfy_v2_processing_api& processing_api() {
    static const dfy_v2_processing_api api = {{sizeof(dfy_v2_processing_api),1},read,start,complete,finish,cancel,payload};
    return api;
}
}
}
