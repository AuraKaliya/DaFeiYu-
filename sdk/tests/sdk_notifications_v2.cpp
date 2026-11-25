#include "fixtures/query_v2/api.h"
#include <windows.h>
#include <array>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace {
// 固定容量的隔离登记及引用表，仅用于通知契约验收。
struct Operation { uint32_t state = DFY_V2_OPERATION_PENDING; };
struct Notice {
    dfy_v2_handle operation{};
    dfy_v2_completion_fn callback = nullptr;
    void* user = nullptr;
    uint32_t state = DFY_V2_NOTIFICATION_PENDING;
    uint64_t sequence = 0;
};
struct Entry {
    uint64_t generation = 1;
    std::shared_ptr<Operation> operation;
    std::shared_ptr<Notice> notice;
};
std::array<Entry, 12> entries;
std::array<std::shared_ptr<Notice>, 2> pending;
bool dispatching = false;
uint64_t next_sequence = 1;
constexpr uint64_t domain = 61;

// 在契约断言失败时结束宿主测试，异常不进入 C ABI。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 核对域、代次及引用是否仍有效。
Entry* resolve(dfy_v2_handle handle) {
    if (handle.domain != domain || !handle.id || handle.id > entries.size()) return nullptr;
    auto& entry = entries[handle.id - 1];
    return entry.generation == handle.generation && (entry.operation || entry.notice) ? &entry : nullptr;
}
// 从固定槽位签发独立引用，耗尽返回空值且不覆盖已有引用。
// 步骤：查找可用代次 → 保存对象引用 → 返回标识或空值。
dfy_v2_handle issue(std::shared_ptr<Operation> operation, std::shared_ptr<Notice> notice) {
    for (size_t i = 0; i < entries.size(); ++i) {
        auto& entry = entries[i];
        if (entry.operation || entry.notice || entry.generation == UINT64_MAX) continue;
        entry.operation = std::move(operation);
        entry.notice = std::move(notice);
        return {domain, i + 1, entry.generation};
    }
    return {};
}
// 返回状态并清空可选错误缓冲。
dfy_v2_status report(dfy_v2_status status, dfy_v2_error* error) {
    if (error) error->length = 0;
    return status;
}
// 保留操作引用，通知登记不可共享。
// 步骤：清空输出 → 检查引用和对象种类 → 签发独立操作引用。
dfy_v2_status DFY_V2_CALL retain(dfy_v2_handle source, dfy_v2_handle* output, dfy_v2_error* error) {
    if (!output) return report(DFY_V2_INVALID_ARGUMENT, error);
    *output = {};
    const auto* entry = resolve(source);
    if (!entry) return report(DFY_V2_INVALID_HANDLE, error);
    if (entry->notice) return report(DFY_V2_UNSUPPORTED, error);
    *output = issue(entry->operation, {});
    return report(output->id ? DFY_V2_OK : DFY_V2_RESOURCE_EXHAUSTED, error);
}
// 仅释放客户端引用；未交付通知仍由 pending 保持，不隐式撤销。
// 步骤：校验引用 → 清空引用槽位 → 推进代次或永久停用槽位。
dfy_v2_status DFY_V2_CALL release(dfy_v2_handle handle, dfy_v2_error* error) {
    if (!handle.domain && !handle.id && !handle.generation) return report(DFY_V2_OK, error);
    auto* entry = resolve(handle);
    if (!entry) return report(DFY_V2_INVALID_HANDLE, error);
    entry->operation.reset();
    entry->notice.reset();
    if (entry->generation != UINT64_MAX) ++entry->generation;
    return report(DFY_V2_OK, error);
}
// 登记一次性通知并保留操作，失败不留容量和引用占用。
// 步骤：检查参数与容量 → 创建宿主记录 → 保留操作并签发登记 → 放入待调度集合。
dfy_v2_status DFY_V2_CALL subscribe(dfy_v2_handle operation, dfy_v2_completion_fn callback,
    void* user, dfy_v2_handle* output, dfy_v2_error* error) {
    if (!output) return report(DFY_V2_INVALID_ARGUMENT, error);
    *output = {};
    if (!callback) return report(DFY_V2_INVALID_ARGUMENT, error);
    const auto* source = resolve(operation);
    if (!source) return report(DFY_V2_INVALID_HANDLE, error);
    if (!source->operation) return report(DFY_V2_INVALID_ARGUMENT, error);
    size_t free = 0;
    while (free < pending.size() && pending[free]) ++free;
    if (free == pending.size() || next_sequence == UINT64_MAX) return report(DFY_V2_RESOURCE_EXHAUSTED, error);

    std::shared_ptr<Notice> notice;
    try { notice = std::make_shared<Notice>(); }
    catch (const std::bad_alloc&) { return report(DFY_V2_RESOURCE_EXHAUSTED, error); }
    auto status = retain(operation, &notice->operation, error);
    if (status != DFY_V2_OK) return status;
    *output = issue({}, notice);
    if (!output->id) {
        release(notice->operation, nullptr);
        return report(DFY_V2_RESOURCE_EXHAUSTED, error);
    }
    notice->callback = callback;
    notice->user = user;
    notice->sequence = next_sequence++;
    pending[free] = std::move(notice);
    return report(DFY_V2_OK, error);
}
// 只读登记状态，错误不返回旧快照。
// 步骤：清空输出 → 核对引用及对象种类 → 复制状态。
dfy_v2_status DFY_V2_CALL read(dfy_v2_handle registration,
    dfy_v2_notification_snapshot* output, dfy_v2_error* error) {
    if (!output) return report(DFY_V2_INVALID_ARGUMENT, error);
    *output = {};
    const auto* entry = resolve(registration);
    if (!entry) return report(DFY_V2_INVALID_HANDLE, error);
    if (!entry->notice) return report(DFY_V2_INVALID_ARGUMENT, error);
    output->state = entry->notice->state;
    return report(DFY_V2_OK, error);
}
// 清除未来入口和内部操作引用，已在当前栈上的记录由调用者保持。
// 步骤：清空回调入口 → 释放借用支撑 → 移除宿主待调度记录。
void detach(const std::shared_ptr<Notice>& notice) {
    notice->callback = nullptr;
    notice->user = nullptr;
    release(notice->operation, nullptr);
    notice->operation = {};
    for (auto& item : pending) if (item == notice) item.reset();
}
// 撤销未开始的通知，对活动调用拒绝且保持状态。
// 步骤：核对登记 → 拒绝活动调用 → 撤销待通知项或幂等返回。
dfy_v2_status DFY_V2_CALL revoke(dfy_v2_handle registration, dfy_v2_error* error) {
    const auto* entry = resolve(registration);
    if (!entry) return report(DFY_V2_INVALID_HANDLE, error);
    if (!entry->notice) return report(DFY_V2_INVALID_ARGUMENT, error);
    const auto notice = entry->notice;
    if (notice->state == DFY_V2_NOTIFICATION_RUNNING) return report(DFY_V2_BUSY, error);
    if (notice->state == DFY_V2_NOTIFICATION_PENDING) {
        notice->state = DFY_V2_NOTIFICATION_REVOKED;
        detach(notice);
    }
    return report(DFY_V2_OK, error);
}
// 手动推进本次边界内的就绪通知；回调中新登记的通知留到下一轮。
// 步骤：拒绝递归推进并固定边界 → 按终态选择回调 → 返回后撤销入口与内部引用。
void pump() {
    check(!dispatching, "nested notification dispatch");
    dispatching = true;
    auto boundary = pending;
    std::sort(boundary.begin(), boundary.end(), [](const auto& left, const auto& right) {
        return left && (!right || left->sequence < right->sequence);
    });
    for (const auto& notice : boundary) {
        if (!notice || notice->state != DFY_V2_NOTIFICATION_PENDING) continue;
        if (resolve(notice->operation)->operation->state == DFY_V2_OPERATION_PENDING) continue;
        notice->state = DFY_V2_NOTIFICATION_RUNNING;
        notice->callback(notice->user, notice->operation);
        notice->state = DFY_V2_NOTIFICATION_DELIVERED;
        detach(notice);
    }
    dispatching = false;
}
const dfy_v2_reference_api refs = {{sizeof(dfy_v2_reference_api), 1}, retain, release};
const dfy_v2_operation_notify_api notifications = {{sizeof(dfy_v2_operation_notify_api), 1}, subscribe, read, revoke};
// 建立覆盖回调使用期的宿主测试上下文。
dfy_test_notify_context context() { return {&notifications, &refs}; }
}

// 通过真实 C 回调验收延后交付、撤销及引用寿命，不提供生产调度器。
// 步骤：加载回调能力 → 验证等待及独立撤销 → 验证就绪后登记和回调中新登记 → 回收引用后释放模块。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 2, "expected fixture DLL");
        using Module = std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(&FreeLibrary)>;
        Module module(LoadLibraryExW(std::filesystem::canonical(argv[1]).c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32), FreeLibrary);
        check(module != nullptr, "cannot load fixture");
        const auto query = reinterpret_cast<dfy_v2_query_fn>(GetProcAddress(module.get(), DFY_V2_QUERY_EXPORT));
        const dfy_v2_plugin_api* api = nullptr;
        check(query && query(2, &api, nullptr) == DFY_V2_OK && api && api->struct_size == sizeof(*api) &&
            api->abi_version == 2 && api->architecture == DFY_V2_ARCH_X64 && !api->reserved && api->query_interface,
            "invalid bootstrap");
        const void* table = nullptr;
        check(api->query_interface({DFY_TEST_NOTIFY_ID, sizeof(DFY_TEST_NOTIFY_ID) - 1}, 1, &table, nullptr) ==
            DFY_V2_OK && table, "missing callback fixture");
        const auto* prefix = static_cast<const dfy_v2_api_header*>(table);
        check(prefix->struct_size == sizeof(dfy_test_notify_api) && prefix->interface_version == 1, "invalid fixture");
        const auto callback = static_cast<const dfy_test_notify_api*>(table)->callback;
        check(callback != nullptr, "missing C callback");

        auto operation = std::make_shared<Operation>();
        const auto source = issue(operation, {});
        auto first = context(), second = context();
        dfy_v2_handle rejected{};
        dfy_v2_notification_snapshot snapshot{99};
        check(subscribe(source, callback, nullptr, nullptr, nullptr) == DFY_V2_INVALID_ARGUMENT &&
            read(source, nullptr, nullptr) == DFY_V2_INVALID_ARGUMENT &&
            read(source, &snapshot, nullptr) == DFY_V2_INVALID_ARGUMENT && !snapshot.state &&
            revoke(source, nullptr) == DFY_V2_INVALID_ARGUMENT, "invalid notification argument accepted");
        std::array<dfy_v2_handle, 11> held{};
        for (auto& handle : held) check(retain(source, &handle, nullptr) == DFY_V2_OK, "cannot fill reference capacity");
        check(subscribe(source, callback, nullptr, &rejected, nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !rejected.id,
            "exhausted references accepted a registration");
        release(held.back(), nullptr);
        check(subscribe(source, callback, nullptr, &rejected, nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !rejected.id,
            "partial registration did not fail");
        for (size_t i = 0; i + 1 < held.size(); ++i) release(held[i], nullptr);
        check(subscribe(source, nullptr, nullptr, &rejected, nullptr) == DFY_V2_INVALID_ARGUMENT && !rejected.id &&
            subscribe({}, callback, nullptr, &rejected, nullptr) == DFY_V2_INVALID_HANDLE, "invalid subscribe accepted");
        check(subscribe(source, callback, &first, &first.registration, nullptr) == DFY_V2_OK &&
            subscribe(source, callback, &second, &second.registration, nullptr) == DFY_V2_OK, "subscribe failed");
        check(!first.calls && !second.calls && retain(first.registration, &rejected, nullptr) == DFY_V2_UNSUPPORTED,
            "synchronous callback or shareable registration");
        check(subscribe(source, callback, nullptr, &rejected, nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !rejected.id,
            "capacity failure leaked registration");
        pump();
        check(!first.calls && !second.calls, "pending operation notified");
        check(revoke(first.registration, nullptr) == DFY_V2_OK && revoke(first.registration, nullptr) == DFY_V2_OK &&
            read(first.registration, &snapshot, nullptr) == DFY_V2_OK && snapshot.state == DFY_V2_NOTIFICATION_REVOKED,
            "revoke not quiescent");
        operation->state = DFY_V2_OPERATION_SUCCEEDED;
        pump(); pump();
        check(!first.calls && second.calls == 1 && second.revoke_status == DFY_V2_BUSY && second.retain_status == DFY_V2_OK,
            "revoked, repeated or unsafe callback");
        check(read(second.registration, &snapshot, nullptr) == DFY_V2_OK && snapshot.state == DFY_V2_NOTIFICATION_DELIVERED &&
            revoke(second.registration, nullptr) == DFY_V2_OK, "callback did not quiesce");
        release(first.registration, nullptr); release(second.registration, nullptr); release(second.retained, nullptr);

        auto removed = context(), older = context(), newer = context();
        uint32_t sequence = 0;
        older.sequence = newer.sequence = &sequence;
        check(subscribe(source, callback, &removed, &removed.registration, nullptr) == DFY_V2_OK &&
            subscribe(source, callback, &older, &older.registration, nullptr) == DFY_V2_OK &&
            revoke(removed.registration, nullptr) == DFY_V2_OK &&
            subscribe(source, callback, &newer, &newer.registration, nullptr) == DFY_V2_OK, "slot reuse failed");
        pump();
        check(!removed.calls && older.observed_order == 1 && newer.observed_order == 2, "slot reuse reordered notifications");
        release(removed.registration, nullptr); release(older.registration, nullptr); release(newer.registration, nullptr);
        release(older.retained, nullptr); release(newer.retained, nullptr);

        auto parent = context(), child = context();
        parent.child = &child;
        check(subscribe(source, callback, &parent, &parent.registration, nullptr) == DFY_V2_OK && !parent.calls,
            "already settled operation reentered subscriber");
        check(release(source, nullptr) == DFY_V2_OK, "cannot release source");
        pump();
        check(parent.calls == 1 && !child.calls && parent.spawn_status == DFY_V2_OK, "new callback ran in same dispatch");
        pump();
        check(child.calls == 1 && child.retain_status == DFY_V2_OK, "callback borrow lost after source release");
        release(parent.registration, nullptr); release(child.registration, nullptr);
        release(parent.retained, nullptr); release(child.retained, nullptr);
        check(read(parent.registration, &snapshot, nullptr) == DFY_V2_INVALID_HANDLE && !snapshot.state &&
            revoke(parent.registration, nullptr) == DFY_V2_INVALID_HANDLE, "stale registration accepted");

        operation = std::make_shared<Operation>();
        const auto fresh = issue(operation, {});
        auto orphan = context();
        check(subscribe(fresh, callback, &orphan, &orphan.registration, nullptr) == DFY_V2_OK, "orphan subscribe failed");
        release(orphan.registration, nullptr); release(fresh, nullptr);
        operation->state = DFY_V2_OPERATION_SUCCEEDED;
        pump();
        check(orphan.calls == 1 && orphan.retain_status == DFY_V2_OK, "client release cancelled notification");
        release(orphan.retained, nullptr);
        for (const auto& entry : entries) check(!entry.operation && !entry.notice, "reference leak");
        for (const auto& item : pending) check(!item, "queued callback remains");
        check(FreeLibrary(module.get()) != 0, "cannot release DLL");
        (void)module.release();
        pump();
        std::cout << "ABI v2 notification checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
