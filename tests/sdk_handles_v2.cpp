#include <cstddef>
#pragma pack(push, 1)
#include "fixtures/query_v2/api.h"
struct HandlePackingProbe { char tag; uint64_t value; };
#pragma pack(pop)
#include <windows.h>
#include <array>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
static_assert(sizeof(HandlePackingProbe) == 9);
static_assert(sizeof(dfy_v2_handle) == 24 && alignof(dfy_v2_handle) == 8 &&
    offsetof(dfy_v2_handle, id) == 8 && offsetof(dfy_v2_handle, generation) == 16);
static_assert(sizeof(dfy_v2_bytes_view) == 16 && alignof(dfy_v2_bytes_view) == 8 &&
    offsetof(dfy_v2_bytes_view, length) == 8);
static_assert(sizeof(dfy_v2_reference_api) == 24 && alignof(dfy_v2_reference_api) == 8 &&
    offsetof(dfy_v2_reference_api, retain) == 8 && offsetof(dfy_v2_reference_api, release) == 16);

namespace {
// 固定两个槽位的隔离宿主仅用于契约验收，不是生产句柄注册表。
struct Slot {
    uint64_t generation = 1;
    std::shared_ptr<const std::string> value;
    bool shareable = true;
};
std::array<Slot, 2> slots;
constexpr uint64_t domain = 41;

// 在契约验收失败时中止测试，异常不进入 C ABI 回调。
void check(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}
// 判断句柄是否为约定的全零空值，不将局部为零的错误标识当作空值。
bool empty(dfy_v2_handle handle) {
    return handle.domain == 0 && handle.id == 0 && handle.generation == 0;
}
// 核对签发域、槽位和代次，仅返回仍持有数据的引用记录。
Slot* resolve(dfy_v2_handle handle) {
    if (handle.domain != domain || handle.id == 0 || handle.id > slots.size()) return nullptr;
    auto& slot = slots[handle.id - 1];
    return slot.value && handle.generation == slot.generation ? &slot : nullptr;
}
// 按允许的零长度诊断约定返回状态，不借用任何 DLL 文本。
dfy_v2_status report(dfy_v2_status status, dfy_v2_error* error) {
    if (error) error->length = 0;
    return status;
}
// 为可共享数据签发独立引用，失败保持原引用和数据不变。
// 步骤：清空输出 → 核对来源和共享能力 → 查找可用代次 → 建立新引用或报告容量不足。
dfy_v2_status DFY_V2_CALL retain(dfy_v2_handle source, dfy_v2_handle* owned, dfy_v2_error* error) {
    if (!owned) return report(DFY_V2_INVALID_ARGUMENT, error);
    *owned = {};
    const auto* original = resolve(source);
    if (!original) return report(DFY_V2_INVALID_HANDLE, error);
    if (!original->shareable) return report(DFY_V2_UNSUPPORTED, error);

    for (size_t i = 0; i < slots.size(); ++i) {
        auto& slot = slots[i];
        if (slot.value || slot.generation == UINT64_MAX) continue;
        slot.value = original->value;
        slot.shareable = true;
        *owned = {domain, i + 1, slot.generation};
        return report(DFY_V2_OK, error);
    }
    return report(DFY_V2_RESOURCE_EXHAUSTED, error);
}
// 释放当前持有引用并使其代次失效，不改变其他引用或触发业务完成。
// 步骤：处理空值或拒绝无效引用 → 释放本引用 → 推进代次或永久停用耗尽槽位。
dfy_v2_status DFY_V2_CALL release(dfy_v2_handle owned, dfy_v2_error* error) {
    if (empty(owned)) return report(DFY_V2_OK, error);
    auto* slot = resolve(owned);
    if (!slot) return report(DFY_V2_INVALID_HANDLE, error);

    slot->value.reset();
    if (slot->generation != UINT64_MAX) ++slot->generation;
    return report(DFY_V2_OK, error);
}
// 返回依赖传入持有引用的只读视图，错误输出清空且不返回失效数据。
dfy_v2_status DFY_V2_CALL view(dfy_v2_handle source, dfy_v2_bytes_view* output) {
    if (!output) return DFY_V2_INVALID_ARGUMENT;
    *output = {};
    const auto* slot = resolve(source);
    if (!slot) return DFY_V2_INVALID_HANDLE;
    *output = {reinterpret_cast<const uint8_t*>(slot->value->data()), slot->value->size()};
    return DFY_V2_OK;
}
const dfy_v2_reference_api refs = {{sizeof(dfy_v2_reference_api), DFY_V2_REFERENCE_VERSION}, retain, release};
}

// 通过真实 C DLL 验证引用转交和借用寿命，并在模块释放后检查宿主数据独立存活。
// 步骤：查询借用夹具 → 验证参数及独立引用 → 拒绝失效句柄 → 卸载 DLL 并完成宿主回收。
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
        check(api->query_interface({DFY_TEST_BORROW_ID, sizeof(DFY_TEST_BORROW_ID) - 1}, 1, &table, nullptr) ==
            DFY_V2_OK && table, "missing borrow capability");
        const auto* prefix = static_cast<const dfy_v2_api_header*>(table);
        check(prefix->struct_size == sizeof(dfy_test_borrow_api) && prefix->interface_version == 1, "invalid borrow table");
        const auto* borrow = static_cast<const dfy_test_borrow_api*>(table);
        check(borrow->preserve != nullptr, "missing preserve function");

        slots[0].value = std::make_shared<const std::string>("A\0B", 3);
        const std::weak_ptr<const std::string> lifetime = slots[0].value;
        const dfy_v2_handle source{domain, 1, 1};
        dfy_v2_handle owned = source;
        dfy_v2_error error{};
        auto bad_refs = refs;
        ++bad_refs.header.interface_version;
        check(borrow->preserve(&bad_refs, view, source, &owned, &error) == DFY_V2_UNSUPPORTED && empty(owned),
            "wrong reference version accepted");
        check(borrow->preserve(nullptr, view, source, &owned, nullptr) == DFY_V2_INVALID_ARGUMENT && empty(owned),
            "missing service accepted");
        check(refs.retain(source, nullptr, nullptr) == DFY_V2_INVALID_ARGUMENT, "null output accepted");
        slots[0].shareable = false;
        check(borrow->preserve(&refs, view, source, &owned, nullptr) == DFY_V2_UNSUPPORTED && empty(owned),
            "non-shareable reference duplicated");
        slots[0].shareable = true;
        check(borrow->preserve(&refs, view, source, &owned, &error) == DFY_V2_OK && error.length == 0 &&
            owned.id != source.id && owned.domain == domain, "C retain failed");
        dfy_v2_handle extra = source;
        check(refs.retain(source, &extra, nullptr) == DFY_V2_RESOURCE_EXHAUSTED && empty(extra), "capacity not enforced");

        dfy_v2_bytes_view bytes{};
        check(refs.release(source, &error) == DFY_V2_OK && !lifetime.expired(), "release destroyed shared data");
        check(refs.retain(owned, &extra, nullptr) == DFY_V2_OK && extra.id == source.id &&
            extra.generation != source.generation, "generation not advanced");
        for (const auto invalid : {source, dfy_v2_handle{domain + 1, owned.id, owned.generation},
            dfy_v2_handle{domain, 0, 1}, dfy_v2_handle{domain, 3, 1}, dfy_v2_handle{domain, owned.id, 0}}) {
            auto rejected = owned;
            check(view(owned, &bytes) == DFY_V2_OK, "live reference lost");
            check(view(invalid, &bytes) == DFY_V2_INVALID_HANDLE && !bytes.data && bytes.length == 0,
                "invalid handle returned a view");
            check(refs.retain(invalid, &rejected, nullptr) == DFY_V2_INVALID_HANDLE && empty(rejected) &&
                refs.release(invalid, nullptr) == DFY_V2_INVALID_HANDLE, "invalid handle mutated references");
        }
        check(refs.release(extra, nullptr) == DFY_V2_OK && refs.release(extra, nullptr) == DFY_V2_INVALID_HANDLE,
            "copied reference released twice");
        check(refs.release({}, nullptr) == DFY_V2_OK && refs.retain({}, &extra, nullptr) == DFY_V2_INVALID_HANDLE &&
            empty(extra), "null handle contract failed");
        slots[0].generation = UINT64_MAX;
        slots[0].value = slots[1].value;
        check(refs.release({domain, 1, UINT64_MAX}, nullptr) == DFY_V2_OK && slots[0].generation == UINT64_MAX,
            "generation wrapped on release");
        check(refs.retain(owned, &extra, nullptr) == DFY_V2_RESOURCE_EXHAUSTED && empty(extra), "retired slot reused");

        check(FreeLibrary(module.get()) != 0, "cannot release fixture reference");
        (void)module.release();
        check(view(owned, &bytes) == DFY_V2_OK && bytes.length == 3 &&
            std::string(reinterpret_cast<const char*>(bytes.data), bytes.length) == std::string("A\0B", 3),
            "host data depended on DLL lifetime");
        check(refs.release(owned, &error) == DFY_V2_OK && error.length == 0 && lifetime.expired(), "data not reclaimed");
        check(view(owned, &bytes) == DFY_V2_INVALID_HANDLE && !bytes.data, "released view was returned");
        std::cout << "ABI v2 handle and borrow checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
