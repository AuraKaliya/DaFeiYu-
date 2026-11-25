#include <cstddef>
#pragma pack(push, 1)
#include "fixtures/query_v2/api.h"
struct OperationPackingProbe { char tag; uint64_t value; };
#pragma pack(pop)
#include <windows.h>
#include <array>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <type_traits>
static_assert(sizeof(OperationPackingProbe) == 9);
static_assert(sizeof(dfy_v2_operation_snapshot) == 4 && alignof(dfy_v2_operation_snapshot) == 4 &&
    offsetof(dfy_v2_operation_snapshot, state) == 0);
static_assert(sizeof(dfy_v2_operation_state_api) == 16 && alignof(dfy_v2_operation_state_api) == 8 &&
    offsetof(dfy_v2_operation_state_api, read) == 8);
static_assert(sizeof(dfy_test_operation_api) == 16 && offsetof(dfy_test_operation_api, read) == 8);
static_assert(sizeof(dfy_v2_cancel_snapshot) == 4 && alignof(dfy_v2_cancel_snapshot) == 4 &&
    offsetof(dfy_v2_cancel_snapshot, reason) == 0);
static_assert(sizeof(dfy_v2_operation_cancel_api) == 24 && alignof(dfy_v2_operation_cancel_api) == 8 &&
    offsetof(dfy_v2_operation_cancel_api, request) == 8 && offsetof(dfy_v2_operation_cancel_api, read) == 16);
static_assert(sizeof(dfy_test_cancel_api) == 24 && offsetof(dfy_test_cancel_api, read) == 16);
static_assert(sizeof(dfy_v2_notification_snapshot) == 4 && alignof(dfy_v2_notification_snapshot) == 4 &&
    offsetof(dfy_v2_notification_snapshot, state) == 0);
static_assert(sizeof(dfy_v2_completion_fn) == 8);
static_assert(sizeof(dfy_v2_operation_notify_api) == 32 && alignof(dfy_v2_operation_notify_api) == 8 &&
    offsetof(dfy_v2_operation_notify_api, subscribe) == 8 && offsetof(dfy_v2_operation_notify_api, read) == 16 &&
    offsetof(dfy_v2_operation_notify_api, revoke) == 24);

namespace {
// 仅用于契约验收的操作记录和两个引用槽，不是生产注册表或执行器。
struct Operation {
    uint32_t state = DFY_V2_OPERATION_PENDING;
    bool is_operation = true;
    bool cancellable = true;
    uint32_t cancel_reason = 0;
};
struct Slot { uint64_t generation = 1; std::shared_ptr<Operation> value; };
std::array<Slot, 2> slots;
std::shared_ptr<Operation> work;
constexpr uint64_t domain = 53;

// 在断言不满足时结束验收，异常不进入 C ABI 回调。
void check(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}
// 判断引用是否为唯一约定的全零空值。
bool empty(dfy_v2_handle handle) {
    return handle.domain == 0 && handle.id == 0 && handle.generation == 0;
}
// 核对签发域、槽位及代次，拒绝已释放的引用。
Slot* resolve(dfy_v2_handle handle) {
    if (handle.domain != domain || handle.id == 0 || handle.id > slots.size()) return nullptr;
    auto& slot = slots[handle.id - 1];
    return slot.value && slot.generation == handle.generation ? &slot : nullptr;
}
// 返回调用状态并清空可选诊断，不将操作失败写成本次调用错误。
dfy_v2_status report(dfy_v2_status status, dfy_v2_error* error) {
    if (error) error->length = 0;
    return status;
}
// 在隔离宿主受理一份工作，引用槽不足或旧工作未回收时拒绝且不改变旧记录。
// 步骤：校验并清空输出 → 核对隔离容量 → 建立宿主工作与首份持有引用。
dfy_v2_status submit(dfy_v2_handle* output) {
    if (!output) return DFY_V2_INVALID_ARGUMENT;
    *output = {};
    if (work || slots[0].value || slots[0].generation == UINT64_MAX) return DFY_V2_RESOURCE_EXHAUSTED;

    work = std::make_shared<Operation>();
    slots[0].value = work;
    *output = {domain, 1, slots[0].generation};
    return DFY_V2_OK;
}
// 为同一次操作签发独立引用，不复制操作状态或改变宿主工作责任。
// 步骤：清空输出并检查来源 → 查找可用槽位 → 建立独立引用或报告容量不足。
dfy_v2_status DFY_V2_CALL retain(dfy_v2_handle source, dfy_v2_handle* output, dfy_v2_error* error) {
    if (!output) return report(DFY_V2_INVALID_ARGUMENT, error);
    *output = {};
    const auto* original = resolve(source);
    if (!original) return report(DFY_V2_INVALID_HANDLE, error);

    for (size_t i = 0; i < slots.size(); ++i) {
        auto& slot = slots[i];
        if (slot.value || slot.generation == UINT64_MAX) continue;
        slot.value = original->value;
        *output = {domain, i + 1, slot.generation};
        return report(DFY_V2_OK, error);
    }
    return report(DFY_V2_RESOURCE_EXHAUSTED, error);
}
// 释放单份引用并使旧标识失效，不取消或回收宿主仍跟踪的工作。
// 步骤：处理空值或拒绝失效引用 → 释放引用 → 推进代次并保持工作记录。
dfy_v2_status DFY_V2_CALL release(dfy_v2_handle owned, dfy_v2_error* error) {
    if (empty(owned)) return report(DFY_V2_OK, error);
    auto* slot = resolve(owned);
    if (!slot) return report(DFY_V2_INVALID_HANDLE, error);

    slot->value.reset();
    if (slot->generation != UINT64_MAX) ++slot->generation;
    return report(DFY_V2_OK, error);
}
// 返回操作状态副本，不推进工作、分配引用或调用消费者。
// 步骤：清空输出 → 校验引用及对象种类 → 复制当前状态。
dfy_v2_status DFY_V2_CALL read_state(dfy_v2_handle operation,
    dfy_v2_operation_snapshot* output, dfy_v2_error* error) {
    if (!output) return report(DFY_V2_INVALID_ARGUMENT, error);
    *output = {};
    const auto* slot = resolve(operation);
    if (!slot) return report(DFY_V2_INVALID_HANDLE, error);
    if (!slot->value->is_operation) return report(DFY_V2_INVALID_ARGUMENT, error);

    output->state = slot->value->state;
    return report(DFY_V2_OK, error);
}
// 由测试宿主手动结算唯一终态；不向消费侧公开结果写入权限。
bool settle(uint32_t terminal) {
    if (!work || work->state != DFY_V2_OPERATION_PENDING ||
        terminal < DFY_V2_OPERATION_SUCCEEDED || terminal > DFY_V2_OPERATION_CANCELLED) return false;
    work->state = terminal;
    return true;
}
const dfy_v2_reference_api refs = {{sizeof(dfy_v2_reference_api), DFY_V2_REFERENCE_VERSION}, retain, release};
const dfy_v2_operation_state_api states = {
    {sizeof(dfy_v2_operation_state_api), DFY_V2_OPERATION_STATE_VERSION}, read_state};

// 只复制已登记的首个取消原因，不混用操作终态。
// 步骤：清空输出 → 核对引用、对象及支持性 → 复制原因。
dfy_v2_status DFY_V2_CALL read_cancel(dfy_v2_handle operation,
    dfy_v2_cancel_snapshot* output, dfy_v2_error* error) {
    if (!output) return report(DFY_V2_INVALID_ARGUMENT, error);
    *output = {};
    const auto* slot = resolve(operation);
    if (!slot) return report(DFY_V2_INVALID_HANDLE, error);
    if (!slot->value->is_operation) return report(DFY_V2_INVALID_ARGUMENT, error);
    if (!slot->value->cancellable) return report(DFY_V2_UNSUPPORTED, error);
    output->reason = slot->value->cancel_reason;
    return report(DFY_V2_OK, error);
}
// 登记协作取消而不结算操作，重复及迟到请求保持既有结果。
// 步骤：清空回执并校验参数 → 核对对象能力 → 按终态或已有请求分流 → 保存首个原因。
dfy_v2_status DFY_V2_CALL request_cancel(dfy_v2_handle operation, uint32_t reason,
    uint32_t* disposition, dfy_v2_error* error) {
    if (!disposition) return report(DFY_V2_INVALID_ARGUMENT, error);
    *disposition = 0;
    if (reason < DFY_V2_CANCEL_USER || reason > DFY_V2_CANCEL_DEADLINE) return report(DFY_V2_INVALID_ARGUMENT, error);
    dfy_v2_cancel_snapshot prior{};
    const auto status = read_cancel(operation, &prior, error);
    if (status != DFY_V2_OK) return status;

    auto& value = *resolve(operation)->value;
    if (value.state != DFY_V2_OPERATION_PENDING) *disposition = DFY_V2_CANCEL_ALREADY_SETTLED;
    else if (prior.reason) *disposition = DFY_V2_CANCEL_ALREADY_REQUESTED;
    else {
        value.cancel_reason = reason;
        *disposition = DFY_V2_CANCEL_RECORDED;
    }
    return report(DFY_V2_OK, error);
}
const dfy_v2_operation_cancel_api cancels = {
    {sizeof(dfy_v2_operation_cancel_api), DFY_V2_OPERATION_CANCEL_VERSION}, request_cancel, read_cancel};

// 通过纯 C 消费者验收取消请求、能力拒绝和两种先后顺序，不启动生产调度器。
// 步骤：检查服务及参数 → 验证首个原因与重复请求 → 验证取消和完成的顺序 → 回收隔离工作。
void verify_cancel(const dfy_test_cancel_api& consumer) {
    dfy_v2_handle operation{};
    check(submit(&operation) == DFY_V2_OK, "cancel submission failed");
    dfy_v2_cancel_snapshot snapshot{99};
    uint32_t disposition = 99;
    dfy_v2_error error{};
    check(consumer.read(&cancels, operation, &snapshot, nullptr) == DFY_V2_OK && !snapshot.reason, "initial cancellation");
    check(consumer.request(nullptr, operation, 1, &disposition, nullptr) == DFY_V2_INVALID_ARGUMENT && !disposition,
        "null cancel service accepted");
    auto invalid = cancels;
    ++invalid.header.interface_version;
    check(consumer.read(&invalid, operation, &snapshot, nullptr) == DFY_V2_UNSUPPORTED && !snapshot.reason,
        "wrong cancel version accepted");
    invalid = cancels;
    ++invalid.header.struct_size;
    check(consumer.request(&invalid, operation, 1, &disposition, nullptr) == DFY_V2_UNSUPPORTED && !disposition,
        "wrong cancel size accepted");
    invalid = cancels;
    invalid.request = nullptr;
    check(consumer.read(&invalid, operation, &snapshot, nullptr) == DFY_V2_INVALID_ARGUMENT, "missing cancel entry");
    check(consumer.request(&cancels, operation, 1, nullptr, nullptr) == DFY_V2_INVALID_ARGUMENT &&
        consumer.read(&cancels, operation, nullptr, nullptr) == DFY_V2_INVALID_ARGUMENT, "null cancel output accepted");
    for (auto reason : {0u, 4u, UINT32_MAX}) {
        disposition = 99;
        check(consumer.request(&cancels, operation, reason, &disposition, nullptr) == DFY_V2_INVALID_ARGUMENT &&
            !disposition && !work->cancel_reason, "invalid reason changed state");
    }
    work->is_operation = false;
    check(consumer.request(&cancels, operation, 1, &disposition, nullptr) == DFY_V2_INVALID_ARGUMENT, "wrong kind cancelled");
    work->is_operation = true;
    work->cancellable = false;
    check(consumer.request(&cancels, operation, 1, &disposition, nullptr) == DFY_V2_UNSUPPORTED && !disposition &&
        !work->cancel_reason, "unsupported operation cancelled");
    work->cancellable = true;

    for (auto first_reason : {DFY_V2_CANCEL_USER, DFY_V2_CANCEL_OWNER_STOP, DFY_V2_CANCEL_DEADLINE}) {
        error.length = 1;
        check(consumer.request(&cancels, operation, first_reason, &disposition, &error) == DFY_V2_OK && !error.length &&
            disposition == DFY_V2_CANCEL_RECORDED && work->state == DFY_V2_OPERATION_PENDING, "request settled work");
        check(consumer.request(&cancels, operation, DFY_V2_CANCEL_USER, &disposition, nullptr) == DFY_V2_OK &&
            disposition == DFY_V2_CANCEL_ALREADY_REQUESTED, "repeat cancellation not idempotent");
        check(consumer.read(&cancels, operation, &snapshot, nullptr) == DFY_V2_OK && snapshot.reason == first_reason,
            "first reason replaced");
        dfy_v2_handle retained{};
        check(refs.retain(operation, &retained, nullptr) == DFY_V2_OK &&
            consumer.read(&cancels, retained, &snapshot, nullptr) == DFY_V2_OK && snapshot.reason == first_reason &&
            refs.release(retained, nullptr) == DFY_V2_OK, "independent reference lost cancellation state");
        // 隔离处理者在业务提交前观察取消；终态与工作回收仍由宿主分别推进。
        check(settle(DFY_V2_OPERATION_CANCELLED) && work != nullptr, "cancelled result lost work responsibility");
        check(consumer.request(&cancels, operation, 1, &disposition, nullptr) == DFY_V2_OK &&
            disposition == DFY_V2_CANCEL_ALREADY_SETTLED && work->cancel_reason == first_reason, "terminal changed");
        check(refs.release(operation, nullptr) == DFY_V2_OK, "cannot release cancelled operation");
        work.reset();
        check(consumer.read(&cancels, operation, &snapshot, nullptr) == DFY_V2_INVALID_HANDLE && !snapshot.reason,
            "released cancel reference accepted");
        check(consumer.request(&cancels, operation, 1, &disposition, nullptr) == DFY_V2_INVALID_HANDLE && !disposition,
            "released operation cancelled");
        check(submit(&operation) == DFY_V2_OK, "next cancellation test not accepted");
    }
    // 取消请求不替代业务结算；已提交或独立失败的工作仍可产生对应正式结果。
    for (auto terminal : {DFY_V2_OPERATION_SUCCEEDED, DFY_V2_OPERATION_FAILED}) {
        check(consumer.request(&cancels, operation, 1, &disposition, nullptr) == DFY_V2_OK && settle(terminal) &&
            work->state == terminal, "cancellation forced a terminal outcome");
        check(refs.release(operation, nullptr) == DFY_V2_OK, "cannot release raced operation");
        work.reset();
        check(submit(&operation) == DFY_V2_OK, "next completion test not accepted");
    }
    check(settle(DFY_V2_OPERATION_SUCCEEDED), "completion first failed");
    check(consumer.request(&cancels, operation, 1, &disposition, nullptr) == DFY_V2_OK &&
        disposition == DFY_V2_CANCEL_ALREADY_SETTLED && !work->cancel_reason &&
        work->state == DFY_V2_OPERATION_SUCCEEDED, "late cancellation overwrote success");
    check(refs.release(operation, nullptr) == DFY_V2_OK, "cannot release success");
    work.reset();
}
}

// 用真实 C DLL 验证操作快照与引用责任，并明确宿主手动结算的验收边界。
// 步骤：查询 C 消费夹具 → 验证状态与引用 → 验证取消请求及先后顺序 → 验证引用释放不取消工作。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 2, "expected v2 fixture DLL");
        using Module = std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(&FreeLibrary)>;
        Module module(LoadLibraryExW(std::filesystem::canonical(argv[1]).c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32), FreeLibrary);
        check(module != nullptr, "cannot load fixture");
        const auto query = reinterpret_cast<dfy_v2_query_fn>(GetProcAddress(module.get(), DFY_V2_QUERY_EXPORT));
        const dfy_v2_plugin_api* api = nullptr;
        check(query && query(2, &api, nullptr) == DFY_V2_OK && api && api->struct_size == sizeof(*api) &&
            api->abi_version == 2 && api->architecture == DFY_V2_ARCH_X64 && api->reserved == 0 && api->query_interface,
            "invalid bootstrap");
        const void* table = nullptr;
        check(api->query_interface({DFY_V2_OPERATION_STATE_INTERFACE, sizeof(DFY_V2_OPERATION_STATE_INTERFACE) - 1},
            1, &table, nullptr) == DFY_V2_UNSUPPORTED && !table, "fixture advertised a host service");
        check(api->query_interface({DFY_TEST_OPERATION_ID, sizeof(DFY_TEST_OPERATION_ID) - 1}, 1, &table, nullptr) ==
            DFY_V2_OK && table, "missing operation fixture");
        const auto* prefix = static_cast<const dfy_v2_api_header*>(table);
        check(prefix->struct_size == sizeof(dfy_test_operation_api) && prefix->interface_version == 1, "invalid fixture");
        const auto* consumer = static_cast<const dfy_test_operation_api*>(table);
        check(consumer->read != nullptr, "missing read function");

        dfy_v2_operation_snapshot snapshot{99};
        dfy_v2_error error{};
        check(consumer->read(nullptr, {}, &snapshot, nullptr) == DFY_V2_INVALID_ARGUMENT && !snapshot.state,
            "missing service accepted");
        for (bool size_mismatch : {false, true}) {
            auto incompatible = states;
            if (size_mismatch) ++incompatible.header.struct_size;
            else ++incompatible.header.interface_version;
            snapshot.state = 99;
            check(consumer->read(&incompatible, {}, &snapshot, &error) == DFY_V2_UNSUPPORTED && !snapshot.state,
                "incompatible service accepted");
        }
        auto incomplete = states;
        incomplete.read = nullptr;
        check(consumer->read(&incomplete, {}, &snapshot, nullptr) == DFY_V2_INVALID_ARGUMENT && !snapshot.state,
            "missing service function accepted");
        check(submit(nullptr) == DFY_V2_INVALID_ARGUMENT && !work, "invalid submission created work");

        for (const auto terminal : {DFY_V2_OPERATION_SUCCEEDED, DFY_V2_OPERATION_FAILED, DFY_V2_OPERATION_CANCELLED}) {
            dfy_v2_handle operation{}, other{};
            check(submit(&operation) == DFY_V2_OK && !empty(operation), "submission failed");
            const std::weak_ptr<Operation> lifetime = work;
            check(submit(&other) == DFY_V2_RESOURCE_EXHAUSTED && empty(other), "rejected submission left a reference");
            check(consumer->read(&states, operation, nullptr, &error) == DFY_V2_INVALID_ARGUMENT, "null output accepted");
            for (int i = 0; i < 2; ++i) {
                error.length = 3;
                check(consumer->read(&states, operation, &snapshot, &error) == DFY_V2_OK && !error.length &&
                    snapshot.state == DFY_V2_OPERATION_PENDING, "query advanced work or confused acceptance with completion");
            }
            const auto pending = snapshot;
            work->is_operation = false;
            check(consumer->read(&states, operation, &snapshot, nullptr) == DFY_V2_INVALID_ARGUMENT && !snapshot.state,
                "wrong object kind accepted");
            work->is_operation = true;
            check(refs.retain(operation, &other, nullptr) == DFY_V2_OK && other.id != operation.id, "retain failed");
            check(refs.release(operation, nullptr) == DFY_V2_OK && !lifetime.expired(), "release discarded work");
            for (const auto invalid : {dfy_v2_handle{}, operation, dfy_v2_handle{domain + 1, other.id, other.generation},
                dfy_v2_handle{domain, 0, 1}, dfy_v2_handle{domain, 3, 1}, dfy_v2_handle{domain, other.id, 0}}) {
                snapshot.state = 99;
                check(consumer->read(&states, invalid, &snapshot, nullptr) == DFY_V2_INVALID_HANDLE && !snapshot.state,
                    "invalid reference returned state");
            }

            check(!settle(0) && !settle(DFY_V2_OPERATION_PENDING) && !settle(99), "invalid terminal accepted");
            check(settle(terminal), "terminal not settled");
            for (const auto repeated : {DFY_V2_OPERATION_SUCCEEDED, DFY_V2_OPERATION_FAILED, DFY_V2_OPERATION_CANCELLED}) {
                check(!settle(repeated), "terminal overwritten");
            }
            work.reset();
            check(!lifetime.expired() && pending.state == DFY_V2_OPERATION_PENDING, "reference or copied snapshot changed");
            error.length = 3;
            check(consumer->read(&states, other, &snapshot, &error) == DFY_V2_OK && !error.length &&
                snapshot.state == terminal, "operation outcome reported as query failure");
            check(refs.release(other, nullptr) == DFY_V2_OK && lifetime.expired(), "finished record not reclaimed");
        }

        check(api->query_interface({DFY_TEST_CANCEL_ID, sizeof(DFY_TEST_CANCEL_ID) - 1}, 1, &table, nullptr) ==
            DFY_V2_OK && table, "missing cancel fixture");
        prefix = static_cast<const dfy_v2_api_header*>(table);
        check(prefix->struct_size == sizeof(dfy_test_cancel_api) && prefix->interface_version == 1, "invalid cancel fixture");
        const auto* cancellation = static_cast<const dfy_test_cancel_api*>(table);
        check(cancellation->request && cancellation->read, "missing cancellation consumer");
        verify_cancel(*cancellation);

        dfy_v2_handle abandoned{};
        check(submit(&abandoned) == DFY_V2_OK && refs.release(abandoned, nullptr) == DFY_V2_OK &&
            work && work->state == DFY_V2_OPERATION_PENDING, "last client release cancelled work");
        check(FreeLibrary(module.get()) != 0, "cannot release fixture reference");
        (void)module.release();
        check(settle(DFY_V2_OPERATION_SUCCEEDED), "host work depended on consumer DLL");
        work.reset();
        check(snapshot.state == DFY_V2_OPERATION_CANCELLED, "snapshot depended on released reference or DLL");
        std::cout << "ABI v2 operation state and cancellation checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
