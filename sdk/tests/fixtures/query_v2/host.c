#include "api.h"
#include <stddef.h>
_Static_assert(sizeof(dfy_v2_type_info) == 32 && offsetof(dfy_v2_type_info, field_count) == 24, "type info layout");
_Static_assert(sizeof(dfy_v2_field_info) == 32 && offsetof(dfy_v2_field_info, max_bytes) == 24, "field info layout");
_Static_assert(sizeof(dfy_v2_type_api) == 24 && offsetof(dfy_v2_type_api, field) == 16, "type table layout");

// 从当前宿主查询生产引用服务并保留来源，不保留宿主表或借用句柄。
// 步骤：清空输出 → 查询并核对引用表 → 取得独立持有。
static dfy_v2_status DFY_V2_CALL preserve(dfy_v2_query_interface_fn query, dfy_v2_handle source,
    dfy_v2_handle* output, dfy_v2_error* error) {
    const void* table = 0;
    const dfy_v2_reference_api* references;
    dfy_v2_status status;
    const dfy_v2_string_view name = {DFY_V2_REFERENCE_INTERFACE, sizeof(DFY_V2_REFERENCE_INTERFACE) - 1};
    if (output) { const dfy_v2_handle empty = {0}; *output = empty; }
    if (error) error->length = 0;
    if (!query || !output) return DFY_V2_INVALID_ARGUMENT;
    status = query(name, 1, &table, error);
    if (status != DFY_V2_OK) return status;
    if (!table) return DFY_V2_FAILED;
    references = (const dfy_v2_reference_api*)table;
    if (references->header.struct_size != sizeof(*references) || references->header.interface_version != 1 ||
        !references->retain || !references->release) return DFY_V2_UNSUPPORTED;
    return references->retain(source, output, error);
}
const dfy_test_host_api dfy_test_host = {{sizeof(dfy_test_host_api), 1}, preserve};

// 通过生产 C 服务反射读取任意最小结构，不依赖名称或宿主 C++ 布局。
// 步骤：查询描述服务 → 检查类型元数据 → 顺序读取字段并汇总种类。
static dfy_v2_status DFY_V2_CALL inspect(dfy_v2_query_interface_fn query, dfy_v2_handle type,
    uint32_t* kinds, dfy_v2_error* error) {
    const dfy_v2_string_view name = {DFY_V2_TYPE_INTERFACE, sizeof(DFY_V2_TYPE_INTERFACE) - 1};
    const void* table = 0;
    const dfy_v2_type_api* types;
    dfy_v2_type_info info = {0};
    dfy_v2_status status;
    uint32_t result = 0;
    uint64_t i;
    if (kinds) *kinds = 0;
    if (error) error->length = 0;
    if (!query || !kinds) return DFY_V2_INVALID_ARGUMENT;
    status = query(name, 1, &table, error);
    if (status != DFY_V2_OK) return status;
    types = (const dfy_v2_type_api*)table;
    if (!types || types->header.struct_size != sizeof(*types) || types->header.interface_version != 1 ||
        !types->describe || !types->field) return DFY_V2_UNSUPPORTED;
    status = types->describe(type, &info, error);
    if (status != DFY_V2_OK) return status;
    if (!info.name.data || !info.name.length || !info.contract_version || info.reserved) return DFY_V2_FAILED;
    for (i = 0; i < info.field_count; ++i) {
        dfy_v2_field_info field = {0};
        status = types->field(type, i, &field, error);
        if (status != DFY_V2_OK) return status;
        if (field.kind < DFY_V2_VALUE_BOOL || field.kind > DFY_V2_VALUE_BYTES || field.reserved) return DFY_V2_FAILED;
        result |= UINT32_C(1) << field.kind;
    }
    *kinds = result;
    return DFY_V2_OK;
}
const dfy_test_types_api dfy_test_types = {{sizeof(dfy_test_types_api), 1}, inspect};
