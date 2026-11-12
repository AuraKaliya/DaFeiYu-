#include "api.h"
#include <stddef.h>
#include <string.h>
_Static_assert(sizeof(dfy_v2_value_view) == 24 && offsetof(dfy_v2_value_view, data) == 8, "value layout");
_Static_assert(sizeof(dfy_v2_value_api) == 16 && sizeof(dfy_v2_struct_api) == 24, "value tables layout");

// 查询并核对宿主能力前缀，测试服务名称都是本地零结尾常量。
static dfy_v2_status load(dfy_v2_query_interface_fn query, const char* name, uint32_t size, const void** output, dfy_v2_error* error) {
    const dfy_v2_string_view id = {name, (uint64_t)strlen(name)};
    dfy_v2_status status = query(id, 1, output, error);
    const dfy_v2_api_header* header;
    if (status != DFY_V2_OK) return status;
    header = (const dfy_v2_api_header*)*output;
    return header && header->struct_size == size && header->interface_version == 1 ? DFY_V2_OK : DFY_V2_UNSUPPORTED;
}
// 仅按种类校验测试正文，不依赖结构名字和字段名。
static int correct(const dfy_v2_value_view* value) {
    if (value->reserved) return 0;
    switch (value->kind) {
    case DFY_V2_VALUE_BOOL: return value->data.boolean == 1;
    case DFY_V2_VALUE_INT64: return value->data.int64 == INT64_MIN;
    case DFY_V2_VALUE_UINT64: return value->data.uint64 == UINT64_MAX;
    case DFY_V2_VALUE_FLOAT64: return value->data.float64 == 1.25;
    case DFY_V2_VALUE_STRING: return value->data.string.length == 3 && value->data.string.data && !memcmp(value->data.string.data, "A\0B", 3);
    case DFY_V2_VALUE_BYTES: return value->data.bytes.length == 3 && value->data.bytes.data && value->data.bytes.data[0] == 0 && value->data.bytes.data[1] == 255;
    default: return 0;
    }
}
// 借助描述逐项读取结构字段，任何结果都释放已经取得的中间持有。
// 步骤：查询并核对服务 → 取得描述 → 逐项读取并释放字段 → 释放描述并返回状态。
static dfy_v2_status DFY_V2_CALL inspect(dfy_v2_query_interface_fn query, dfy_v2_handle structure, dfy_v2_error* error) {
    const void* table = 0;
    const dfy_v2_reference_api* refs;
    const dfy_v2_type_api* types;
    const dfy_v2_value_api* values;
    const dfy_v2_struct_api* structures;
    dfy_v2_handle type = {0};
    dfy_v2_type_info info = {0};
    dfy_v2_status status;
    uint64_t i;
    if (error) error->length = 0;
    if (!query) return DFY_V2_INVALID_ARGUMENT;
    status = load(query, DFY_V2_REFERENCE_INTERFACE, sizeof(*refs), &table, error); if (status) return status;
    refs = (const dfy_v2_reference_api*)table;
    status = load(query, DFY_V2_TYPE_INTERFACE, sizeof(*types), &table, error); if (status) return status;
    types = (const dfy_v2_type_api*)table;
    status = load(query, DFY_V2_VALUE_INTERFACE, sizeof(*values), &table, error); if (status) return status;
    values = (const dfy_v2_value_api*)table;
    status = load(query, DFY_V2_STRUCT_INTERFACE, sizeof(*structures), &table, error); if (status) return status;
    structures = (const dfy_v2_struct_api*)table;
    if (!refs->retain || !refs->release || !types->describe || !types->field || !values->read ||
        !structures->descriptor || !structures->field) return DFY_V2_UNSUPPORTED;
    status = structures->descriptor(structure, &type, error); if (status) return status;
    status = types->describe(type, &info, error);
    for (i = 0; status == DFY_V2_OK && i < info.field_count; ++i) {
        dfy_v2_handle field = {0};
        dfy_v2_value_view value = {0};
        status = structures->field(structure, i, &field, error);
        if (status) break;
        status = values->read(field, &value, error);
        if (!status && !correct(&value)) status = DFY_V2_FAILED;
        refs->release(field, 0);
    }
    refs->release(type, 0);
    return status;
}
const dfy_test_values_api dfy_test_values = {{sizeof(dfy_test_values_api), 1}, inspect};
