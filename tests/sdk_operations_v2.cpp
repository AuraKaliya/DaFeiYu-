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

namespace {
// 仅用于契约验收的操作记录和两个引用槽，不是生产注册表或执行器。
struct Operation { uint32_t state = DFY_V2_OPERATION_PENDING; bool is_operation = true; };
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
}

// 用真实 C DLL 验证操作快照与引用责任，并明确宿主手动结算的验收边界。
// 步骤：查询 C 消费夹具 → 验证服务和句柄拒绝 → 遍历唯一终态及独立引用 → 验证引用释放不取消工作。
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

        dfy_v2_handle abandoned{};
        check(submit(&abandoned) == DFY_V2_OK && refs.release(abandoned, nullptr) == DFY_V2_OK &&
            work && work->state == DFY_V2_OPERATION_PENDING, "last client release cancelled work");
        check(FreeLibrary(module.get()) != 0, "cannot release fixture reference");
        (void)module.release();
        check(settle(DFY_V2_OPERATION_SUCCEEDED), "host work depended on consumer DLL");
        work.reset();
        check(snapshot.state == DFY_V2_OPERATION_CANCELLED, "snapshot depended on released reference or DLL");
        std::cout << "ABI v2 operation state checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
