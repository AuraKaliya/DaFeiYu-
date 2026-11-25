#include <cstddef>
#pragma pack(push, 1)
#include <dafeiyu/plugin_abi.h>
struct PackingProbe { char tag; uint64_t value; };
static_assert(sizeof(PackingProbe) == 9);
#pragma pack(pop)
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
static_assert(sizeof(dfy_string_view) == 16 && alignof(dfy_string_view) == 8);
static_assert(sizeof(dfy_error) == 260 && alignof(dfy_error) == 4);
static_assert(sizeof(dfy_event) == 24 && offsetof(dfy_event, turn_id) == 16);
static_assert(sizeof(dfy_host_api) == 40 && offsetof(dfy_host_api, subscribe) == 16);
static_assert(sizeof(dfy_plugin_api) == 48 && offsetof(dfy_plugin_api, destroy) == 40);

namespace {
// 在 C/C++ 契约或实际 DLL 查询不符合预期时报告失败。
void check(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}
}
extern "C" {
// 测试宿主不提供事件订阅，输出零标识并明确拒绝。
static dfy_status DFY_CALL subscribe(dfy_host_context*, dfy_event_callback, void*, uint64_t* token, dfy_error* error) {
    if (token) *token = 0;
    if (error) error->length = 0;
    return DFY_UNSUPPORTED;
}
// 测试宿主没有订阅，取消操作始终返回未找到。
static dfy_status DFY_CALL unsubscribe(dfy_host_context*, uint64_t, dfy_error* error) {
    if (error) error->length = 0;
    return DFY_NOT_FOUND;
}
// 查询测试不展示插件诊断，借用文本在返回后不保留。
static void DFY_CALL diagnostic(dfy_host_context*, dfy_string_view) {}
}
struct dfy_host_context { int unused; };

// 用 C++ 宿主查询纯 C DLL，检查版本协商和调用方拥有的错误缓冲。
// 步骤：加载指定测试 DLL → 核对查询与接口布局 → 检查失败输出 → 释放本次模块引用。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 2, "expected DLL path");
        auto path = std::filesystem::canonical(argv[1]);
        path.make_preferred();
        using Module = std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(&FreeLibrary)>;
        Module module(LoadLibraryExW(path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32), FreeLibrary);
        check(module != nullptr, "cannot load query DLL");
        const auto query = reinterpret_cast<dfy_query_fn>(GetProcAddress(module.get(), DFY_QUERY_EXPORT));
        check(query != nullptr, "fixed C export missing");

        const dfy_plugin_api* api = nullptr;
        dfy_error error{};
        check(query(DFY_ABI_VERSION, &api, &error) == DFY_OK && api && error.length == 0, "query failed");
        check(api->struct_size == sizeof(*api) && api->abi_version == DFY_ABI_VERSION &&
                  api->architecture == DFY_ARCH_X64 && api->reserved == 0 &&
                  api->create && api->start && api->stop && api->destroy, "API table incomplete");
        const auto* first = api;
        check(query(2, &api, &error) == DFY_ABI_MISMATCH && !api && error.length > 0 &&
                  error.length <= DFY_ERROR_CAPACITY, "version failure retained table or invalid error");
        check(query(1, nullptr, nullptr) == DFY_INVALID_ARGUMENT, "null output accepted");
        check(query(1, &api, &error) == DFY_OK && api == first && error.length == 0, "query not repeatable");

        dfy_host_context context{};
        const dfy_host_api host{sizeof(dfy_host_api), DFY_ABI_VERSION, &context, subscribe, unsubscribe, diagnostic};
        dfy_plugin_instance* instance = reinterpret_cast<dfy_plugin_instance*>(&context);
        check(api->create(&host, {"{}", 2}, &instance, &error) == DFY_UNSUPPORTED && !instance &&
                  error.length > 0 && error.length <= DFY_ERROR_CAPACITY, "query-only instance contract failed");
        const std::string copied_error(error.text, error.length);
        check(FreeLibrary(module.get()) != 0, "cannot release test module reference");
        (void)module.release();
        check(std::string(error.text, error.length) == copied_error, "error depended on DLL storage");
        std::cout << "C/C++ plugin ABI checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
