#include <cstddef>
#pragma pack(push, 1)
#include <dafeiyu/plugin_abi.h>
#include "fixtures/query_v2/api.h"
struct PackingProbe { char tag; uint64_t value; };
#pragma pack(pop)
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
static_assert(sizeof(PackingProbe) == 9);
static_assert(sizeof(dfy_plugin_api) == 48 && DFY_ABI_VERSION == 1);
static_assert(sizeof(dfy_v2_string_view) == 16 && alignof(dfy_v2_string_view) == 8 &&
    offsetof(dfy_v2_string_view, length) == 8);
static_assert(sizeof(dfy_v2_error) == 260 && alignof(dfy_v2_error) == 4 && offsetof(dfy_v2_error, text) == 4);
static_assert(sizeof(dfy_v2_api_header) == 8 && alignof(dfy_v2_api_header) == 4 &&
    offsetof(dfy_v2_api_header, interface_version) == 4);
static_assert(sizeof(dfy_v2_plugin_api) == 24 && alignof(dfy_v2_plugin_api) == 8 &&
    offsetof(dfy_v2_plugin_api, query_interface) == 16);
static_assert(sizeof(dfy_test_query_api) == 16 && offsetof(dfy_test_query_api, header) == 0 &&
    offsetof(dfy_test_query_api, value) == 8);

namespace {
// 在 C ABI 布局、查询协商或引用寿命不符合约定时报告失败。
void check(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}
}

// 使用真实 C DLL 验证独立查询入口、精确能力协商与调用方错误缓冲。
// 步骤：加载并查询引导表 → 校验能力及参数边界 → 重复查询 → 释放引用并核对诊断寿命。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 2, "expected v2 fixture DLL");
        const auto path = std::filesystem::canonical(argv[1]);
        using Module = std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(&FreeLibrary)>;
        Module module(LoadLibraryExW(path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32), FreeLibrary);
        check(module != nullptr, "cannot load fixture");
        const auto query = reinterpret_cast<dfy_v2_query_fn>(GetProcAddress(module.get(), DFY_V2_QUERY_EXPORT));
        check(query && !GetProcAddress(module.get(), DFY_QUERY_EXPORT), "query export is not independent");
        const dfy_v2_plugin_api* api = nullptr;
        dfy_v2_error error{};
        for (uint32_t version : {0u, 1u, 3u}) {
            api = reinterpret_cast<const dfy_v2_plugin_api*>(1);
            check(query(version, &api, &error) == DFY_V2_ABI_MISMATCH && !api &&
                error.length > 0 && error.length <= DFY_V2_ERROR_CAPACITY, "ABI mismatch contract failed");
        }
        check(query(2, nullptr, &error) == DFY_V2_INVALID_ARGUMENT, "null output accepted");
        check(query(2, &api, &error) == DFY_V2_OK && api && error.length == 0, "query failed");
        check(api->struct_size == sizeof(*api) && api->abi_version == 2 &&
            api->architecture == DFY_V2_ARCH_X64 && api->reserved == 0 && api->query_interface,
            "invalid bootstrap metadata");

        const char name[] = DFY_TEST_QUERY_ID ".suffix";
        const dfy_v2_string_view id{name, sizeof(DFY_TEST_QUERY_ID) - 1};
        const void* table = nullptr;
        check(api->query_interface(id, 1, &table, &error) == DFY_V2_OK && table && error.length == 0,
            "length-delimited capability query failed");
        const auto* prefix = static_cast<const dfy_v2_api_header*>(table);
        check(prefix->struct_size == sizeof(dfy_test_query_api) && prefix->interface_version == 1,
            "invalid capability prefix");
        const auto* capability = static_cast<const dfy_test_query_api*>(table);
        check(capability->value && capability->value() == 42, "C capability call failed");
        for (uint32_t version : {0u, 2u}) {
            table = capability;
            check(api->query_interface(id, version, &table, &error) ==
                (version == 0 ? DFY_V2_INVALID_ARGUMENT : DFY_V2_UNSUPPORTED) && !table,
                "capability version rejection failed");
        }
        for (const dfy_v2_string_view invalid : {dfy_v2_string_view{nullptr, 1}, {name, 0}}) {
            table = capability;
            check(api->query_interface(invalid, 1, &table, nullptr) == DFY_V2_INVALID_ARGUMENT && !table,
                "invalid name accepted");
        }
        check(api->query_interface(id, 1, nullptr, nullptr) == DFY_V2_INVALID_ARGUMENT, "null capability output accepted");
        for (const dfy_v2_string_view unknown : {dfy_v2_string_view{name, sizeof(name) - 1}, {"Test.query", 10}}) {
            table = capability;
            check(api->query_interface(unknown, 1, &table, &error) == DFY_V2_UNSUPPORTED && !table &&
                error.length > 0 && error.length <= DFY_V2_ERROR_CAPACITY, "unknown capability accepted");
        }

        const auto* first = api;
        check(query(2, &api, nullptr) == DFY_V2_OK && api == first, "bootstrap query changed state");
        check(api->query_interface(id, 1, &table, nullptr) == DFY_V2_OK && table == capability,
            "capability query changed state");
        const std::string diagnostic(error.text, error.length);
        check(FreeLibrary(module.get()) != 0, "cannot release fixture reference");
        (void)module.release();
        check(std::string(error.text, error.length) == diagnostic, "error buffer borrowed DLL memory");
        std::cout << "ABI v2 query checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
