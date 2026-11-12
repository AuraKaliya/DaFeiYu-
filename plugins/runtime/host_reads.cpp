#include "host_state.hpp"

namespace dfy::plugins::detail {
namespace {
// 先准备边界引用再接受读取，所有可预期分配失败都发生在消费队列前。
// 步骤：清空输出 → 校验控制器 → 预留引用 → 提交读取或回滚 → 返回完整请求身份。
dfy_v2_status DFY_V2_CALL next(dfy_v2_handle source, dfy_v2_handle* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        const auto reader = std::get<std::shared_ptr<events::SubscriptionReader>>(host.references.get(native(source)).object);
        auto reference = std::make_shared<HostReference>(HostReference{std::shared_ptr<events::ReadRequest>{}});
        const auto reserved = host.references.insert(reference);
        try { reference->object = reader->next(); }
        catch (const std::length_error&) { host.references.release(reserved); return DFY_V2_RESOURCE_EXHAUSTED; }
        catch (const std::logic_error&) { host.references.release(reserved); return DFY_V2_BUSY; }
        catch (...) { host.references.release(reserved); throw; }
        *output = abi(reserved);
        return DFY_V2_OK;
    });
}
// 返回已经结算的类型化结果，流结束与取消等待保持独立。
// 步骤：清空输出 → 拒绝 Pending → 显式映射结果与关闭模式 → 复制事件身份。
dfy_v2_status DFY_V2_CALL result(dfy_v2_handle source, dfy_v2_read_result* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        const auto current = request(host, source);
        if (current->operation()->state() == runtime::OperationState::pending) return DFY_V2_BUSY;
        const auto snapshot = current->result();
        dfy_v2_read_result read{};
        switch (snapshot.outcome) {
        case events::ReadOutcome::event: {
            read.outcome = DFY_V2_READ_EVENT;
            const auto id = current->event().id;
            read.event_domain = id.domain; read.event_sequence = id.sequence;
            break;
        }
        case events::ReadOutcome::closed:
            read.outcome = DFY_V2_READ_CLOSED;
            read.close_mode = snapshot.close_mode == events::CloseMode::drain ? DFY_V2_CLOSE_DRAIN : DFY_V2_CLOSE_CANCEL;
            break;
        case events::ReadOutcome::cancelled: read.outcome = DFY_V2_READ_CANCELLED; break;
        }
        *output = read;
        return DFY_V2_OK;
    });
}
// 以完整请求为别名所有者，载荷引用继续占用事件存储及结果槽预算。
dfy_v2_status DFY_V2_CALL payload(dfy_v2_handle source, dfy_v2_handle* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        const auto current = request(host, source);
        if (current->operation()->state() == runtime::OperationState::pending) return DFY_V2_BUSY;
        if (current->result().outcome != events::ReadOutcome::event) return DFY_V2_INVALID_ARGUMENT;
        *output = store_object(host, std::shared_ptr<const runtime::StructValue>(current, current->event().payload.get()));
        return DFY_V2_OK;
    });
}
// 显式映射操作终态，不将 C++ 枚举数值直接暴露。
dfy_v2_status DFY_V2_CALL state(dfy_v2_handle source, dfy_v2_operation_snapshot* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        switch (request(host, source)->operation()->state()) {
        case runtime::OperationState::pending: output->state = DFY_V2_OPERATION_PENDING; break;
        case runtime::OperationState::succeeded: output->state = DFY_V2_OPERATION_SUCCEEDED; break;
        case runtime::OperationState::failed: output->state = DFY_V2_OPERATION_FAILED; break;
        case runtime::OperationState::cancelled: output->state = DFY_V2_OPERATION_CANCELLED; break;
        }
        return DFY_V2_OK;
    });
}
}
// 提供固定的类型化读取能力。
const dfy_v2_reader_api& reader_api() {
    static const dfy_v2_reader_api api = {{sizeof(dfy_v2_reader_api), 1}, next, result, payload};
    return api;
}
// 提供已有 ABI 契约对应的生产操作状态服务。
const dfy_v2_operation_state_api& operation_api() {
    static const dfy_v2_operation_state_api api = {{sizeof(dfy_v2_operation_state_api), 1}, state};
    return api;
}
}
