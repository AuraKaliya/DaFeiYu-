#include <stddef.h>
#include <string.h>
#pragma pack(push, 1)
#include "api.h"
struct packing_probe { char tag; uint64_t value; };
#pragma pack(pop)
_Static_assert(sizeof(struct packing_probe) == 9, "SDK restores caller packing");
_Static_assert(sizeof(dfy_v2_string_view) == 16 && _Alignof(dfy_v2_string_view) == 8 &&
    offsetof(dfy_v2_string_view, length) == 8, "view layout");
_Static_assert(sizeof(dfy_v2_error) == 260 && _Alignof(dfy_v2_error) == 4 &&
    offsetof(dfy_v2_error, text) == 4, "error layout");
_Static_assert(sizeof(dfy_v2_api_header) == 8 && _Alignof(dfy_v2_api_header) == 4 &&
    offsetof(dfy_v2_api_header, interface_version) == 4, "prefix layout");
_Static_assert(sizeof(dfy_v2_plugin_api) == 24 && _Alignof(dfy_v2_plugin_api) == 8 &&
    offsetof(dfy_v2_plugin_api, query_interface) == 16, "bootstrap layout");
_Static_assert(sizeof(dfy_test_query_api) == 16 && offsetof(dfy_test_query_api, value) == 8, "test table layout");
_Static_assert(sizeof(dfy_v2_enum_info) == 32 && offsetof(dfy_v2_enum_info, member_count) == 24, "enum info layout");
_Static_assert(sizeof(dfy_v2_enum_member) == 40 && offsetof(dfy_v2_enum_member, number) == 16, "enum member layout");
_Static_assert(sizeof(dfy_v2_enum_selection) == 32 && offsetof(dfy_v2_enum_selection, number) == 8, "enum selection layout");
_Static_assert(sizeof(dfy_v2_enum_api) == 48 && offsetof(dfy_v2_enum_api, read) == 40, "enum table layout");
_Static_assert(sizeof(dfy_v2_constraint_api) == 48 && offsetof(dfy_v2_constraint_api, map) == 40, "constraint table layout");
_Static_assert(sizeof(dfy_v2_constraint_api_v2) == 56 && offsetof(dfy_v2_constraint_api_v2, element) == 48, "constraint v2 table layout");
_Static_assert(sizeof(dfy_v2_array_api_v2) == 40 && offsetof(dfy_v2_array_api_v2, element_type) == 32, "array v2 table layout");
_Static_assert(sizeof(dfy_v2_map_api_v2) == 48 && offsetof(dfy_v2_map_api_v2, value_type) == 40, "map v2 table layout");
_Static_assert(sizeof(dfy_v2_group_member_info) == 16 && offsetof(dfy_v2_group_member_info, queued) == 8, "group member layout");
_Static_assert(sizeof(dfy_v2_group_api) == 32 && offsetof(dfy_v2_group_api, read) == 24, "group table layout");
_Static_assert(sizeof(dfy_v2_union_info) == 32 && offsetof(dfy_v2_union_info, branch_count) == 24, "union info layout");
_Static_assert(sizeof(dfy_v2_union_selection) == 16 && offsetof(dfy_v2_union_selection, tag) == 8, "union selection layout");
_Static_assert(sizeof(dfy_v2_union_api) == 48 && offsetof(dfy_v2_union_api, content) == 40, "union table layout");

// 将固定诊断写入调用方缓冲；本夹具文本均为容量以内的 ASCII，不依赖 DLL 内文本寿命。
static dfy_v2_status report(dfy_v2_status status, dfy_v2_error* error, const char* message) {
    if (error) {
        error->length = (uint32_t)strlen(message);
        memcpy(error->text, message, error->length);
    }
    return status;
}

// 提供可实际调用的测试能力，证明查询所得函数地址与 C 调用约定可用。
static uint32_t DFY_V2_CALL value(void) { return UINT32_C(42); }
static const dfy_test_query_api test_api = {{sizeof(dfy_test_query_api), 1}, value};

// 精确匹配能力名称及版本，未知能力不产生任何注册或实例。
// 步骤：清空输出 → 校验必需参数 → 按长度和版本匹配 → 返回静态表或不支持。
static dfy_v2_status DFY_V2_CALL query_interface(
    dfy_v2_string_view name, uint32_t version, const void** output, dfy_v2_error* error) {
    if (output) *output = NULL;
    if (!output || !name.data || name.length == 0 || version == 0) {
        return report(DFY_V2_INVALID_ARGUMENT, error, "invalid interface query");
    }

    if (version == 1) {
        if (name.length == sizeof(DFY_TEST_QUERY_ID) - 1 && memcmp(name.data, DFY_TEST_QUERY_ID, name.length) == 0)
            *output = &test_api;
        if (name.length == sizeof(DFY_TEST_BORROW_ID) - 1 && memcmp(name.data, DFY_TEST_BORROW_ID, name.length) == 0)
            *output = &dfy_test_borrow;
        if (name.length == sizeof(DFY_TEST_OPERATION_ID) - 1 && memcmp(name.data, DFY_TEST_OPERATION_ID, name.length) == 0)
            *output = &dfy_test_operation;
        if (name.length == sizeof(DFY_TEST_CANCEL_ID) - 1 && memcmp(name.data, DFY_TEST_CANCEL_ID, name.length) == 0)
            *output = &dfy_test_cancel;
        if (name.length == sizeof(DFY_TEST_NOTIFY_ID) - 1 && memcmp(name.data, DFY_TEST_NOTIFY_ID, name.length) == 0)
            *output = &dfy_test_notify;
        if (name.length == sizeof(DFY_TEST_HOST_ID) - 1 && memcmp(name.data, DFY_TEST_HOST_ID, name.length) == 0)
            *output = &dfy_test_host;
        if (name.length == sizeof(DFY_TEST_TYPES_ID) - 1 && memcmp(name.data, DFY_TEST_TYPES_ID, name.length) == 0)
            *output = &dfy_test_types;
        if (name.length == sizeof(DFY_TEST_VALUES_ID) - 1 && memcmp(name.data, DFY_TEST_VALUES_ID, name.length) == 0)
            *output = &dfy_test_values;
        if (name.length == sizeof(DFY_TEST_READS_ID) - 1 && memcmp(name.data, DFY_TEST_READS_ID, name.length) == 0)
            *output = &dfy_test_reads;
        if (name.length == sizeof(DFY_V2_LIFECYCLE_INTERFACE) - 1 && memcmp(name.data, DFY_V2_LIFECYCLE_INTERFACE, name.length) == 0)
            *output = &dfy_test_lifecycle;
        if (name.length == sizeof(DFY_TEST_NESTED_ID) - 1 && memcmp(name.data, DFY_TEST_NESTED_ID, name.length) == 0)
            *output = &dfy_test_nested;
        if (name.length == sizeof(DFY_TEST_PRESENCE_ID) - 1 && memcmp(name.data, DFY_TEST_PRESENCE_ID, name.length) == 0)
            *output = &dfy_test_presence;
        if (name.length == sizeof(DFY_TEST_ARRAYS_ID) - 1 && memcmp(name.data, DFY_TEST_ARRAYS_ID, name.length) == 0)
            *output = &dfy_test_arrays;
        if (name.length == sizeof(DFY_TEST_MAPS_ID) - 1 && memcmp(name.data, DFY_TEST_MAPS_ID, name.length) == 0)
            *output = &dfy_test_maps;
        if (name.length == sizeof(DFY_TEST_ENUMS_ID) - 1 && memcmp(name.data, DFY_TEST_ENUMS_ID, name.length) == 0)
            *output = &dfy_test_enums;
        if (name.length == sizeof(DFY_TEST_UNIONS_ID) - 1 && memcmp(name.data, DFY_TEST_UNIONS_ID, name.length) == 0)
            *output = &dfy_test_unions;
        if (name.length == sizeof(DFY_TEST_COMPOSITE_ARRAYS_ID) - 1 && memcmp(name.data, DFY_TEST_COMPOSITE_ARRAYS_ID, name.length) == 0)
            *output = &dfy_test_composite_arrays;
        if (name.length == sizeof(DFY_TEST_COMPOSITE_MAPS_ID) - 1 && memcmp(name.data, DFY_TEST_COMPOSITE_MAPS_ID, name.length) == 0)
            *output = &dfy_test_composite_maps;
    }
    if (!*output) return report(DFY_V2_UNSUPPORTED, error, "unsupported interface or version");
    return report(DFY_V2_OK, error, "");
}
static const dfy_v2_plugin_api api = {sizeof(dfy_v2_plugin_api), DFY_V2_ABI_VERSION,
    DFY_V2_ARCH_X64, 0, query_interface};

// 通过独立导出提供 v2 引导表，不实现插件实例生命周期。
// 步骤：清空输出 → 校验输出及主 ABI → 返回静态引导表。
DFY_V2_EXPORT dfy_v2_status DFY_V2_CALL dfy_plugin_query_v2(
    uint32_t requested_abi, const dfy_v2_plugin_api** output, dfy_v2_error* error) {
    if (output) *output = NULL;
    if (!output) return report(DFY_V2_INVALID_ARGUMENT, error, "API output required");
    if (requested_abi != DFY_V2_ABI_VERSION) {
        return report(DFY_V2_ABI_MISMATCH, error, "unsupported ABI version");
    }

    *output = &api;
    return report(DFY_V2_OK, error, "");
}
