#include "api.h"
#include <stddef.h>
#include <string.h>
_Static_assert(sizeof(dfy_v2_type_api_v2) == 32 && offsetof(dfy_v2_type_api_v2, nested) == 24, "nested type layout");
_Static_assert(sizeof(dfy_v2_value_api_v2) == 24 && offsetof(dfy_v2_value_api_v2, structure) == 16, "nested value layout");
_Static_assert(sizeof(dfy_v2_struct_api_v2) == 24, "nested structure layout");
_Static_assert(sizeof(dfy_v2_field_info_v2) == 32 && offsetof(dfy_v2_field_info_v2, flags) == 20, "extended field layout");
typedef struct services {
    const dfy_v2_reference_api* refs;
    const dfy_v2_type_api_v2* types;
    const dfy_v2_value_api_v2* values;
    const dfy_v2_struct_api_v2* structures;
} services;
// 查询精确能力版本，未知或错误布局不作为兼容结果使用。
static const void* service(dfy_v2_query_interface_fn query, const char* name, uint32_t version, uint32_t size) {
    const dfy_v2_string_view id = {name, (uint64_t)strlen(name)};
    const void* table = 0;
    const dfy_v2_api_header* header;
    if (query(id, version, &table, 0)) return 0;
    header = (const dfy_v2_api_header*)table;
    return header && header->struct_size == size && header->interface_version == version ? table : 0;
}
// 在有界深度内遍历结构，每层归还自身取得的字段、描述和结构引用。
// 步骤：检查预算并读取描述 → 枚举字段分类 → 递归结构或计量基础值 → 统一归还持有。
static dfy_v2_status walk(const services* api, dfy_v2_handle structure, uint32_t depth, dfy_test_nested_counts* count) {
    dfy_v2_handle type = {0};
    dfy_v2_type_info info = {0};
    dfy_v2_status status;
    uint64_t i;
    if (!depth) return DFY_V2_RESOURCE_EXHAUSTED;
    status = api->structures->descriptor(structure, &type, 0);
    if (status) return status;
    status = api->types->describe(type, &info, 0);

    for (i = 0; !status && i < info.field_count; ++i) {
        dfy_v2_handle field = {0}, child = {0}, schema = {0};
        dfy_v2_field_info_v2 description = {0};
        dfy_v2_value_view value = {0};
        status = api->types->field(type, i, &description, 0);
        if (!status && description.flags) status = DFY_V2_UNSUPPORTED;
        if (!status) status = api->structures->field(structure, i, &field, 0);
        if (!status) status = api->values->read(field, &value, 0);
        if (!status && value.kind != description.kind) status = DFY_V2_FAILED;
        if (!status && value.kind == DFY_V2_VALUE_STRUCTURE) {
            dfy_v2_type_info nested = {0};
            if (value.data.uint64 || value.data.bytes.length) status = DFY_V2_FAILED;
            if (!status) status = api->types->nested(type, i, &schema, 0);
            if (!status) status = api->types->describe(schema, &nested, 0);
            if (!status && !nested.contract_version) status = DFY_V2_FAILED;
            if (!status) status = api->values->structure(field, &child, 0);
            if (!status) status = walk(api, child, depth - 1, count);
        } else if (!status && value.kind >= DFY_V2_VALUE_BOOL && value.kind <= DFY_V2_VALUE_BYTES) {
            ++count->leaves;
            if (value.kind == DFY_V2_VALUE_STRING) count->bytes += value.data.string.length;
            if (value.kind == DFY_V2_VALUE_BYTES) count->bytes += value.data.bytes.length;
        } else if (!status) status = DFY_V2_UNSUPPORTED;

        api->refs->release(schema, 0); api->refs->release(child, 0); api->refs->release(field, 0);
    }

    api->refs->release(type, 0);
    return status;
}
// 通过纯 C 的版本协商和递归访问读取结构，不预编译具体类型或字段名。
// 步骤：清空输出 → 查询完整能力 → 检查必需函数 → 在预算内读取并提交统计。
static dfy_v2_status DFY_V2_CALL inspect(dfy_v2_query_interface_fn query, dfy_v2_handle structure,
    uint32_t depth, dfy_test_nested_counts* output, dfy_v2_error* error) {
    services api;
    dfy_test_nested_counts count = {0};
    dfy_v2_status status;
    if (output) *output = count;
    if (error) error->length = 0;
    if (!query || !output) return DFY_V2_INVALID_ARGUMENT;

    api.refs = (const dfy_v2_reference_api*)service(query, DFY_V2_REFERENCE_INTERFACE, 1, sizeof(*api.refs));
    api.types = (const dfy_v2_type_api_v2*)service(query, DFY_V2_TYPE_INTERFACE, 2, sizeof(*api.types));
    api.values = (const dfy_v2_value_api_v2*)service(query, DFY_V2_VALUE_INTERFACE, 2, sizeof(*api.values));
    api.structures = (const dfy_v2_struct_api_v2*)service(query, DFY_V2_STRUCT_INTERFACE, 2, sizeof(*api.structures));
    if (!api.refs || !api.types || !api.values || !api.structures || !api.refs->release || !api.types->describe ||
        !api.types->field || !api.types->nested || !api.values->read || !api.values->structure ||
        !api.structures->descriptor || !api.structures->field) return DFY_V2_UNSUPPORTED;

    status = walk(&api, structure, depth, &count);
    if (!status) *output = count;
    return status;
}
const dfy_test_nested_api dfy_test_nested = {{sizeof(dfy_test_nested_api), 1}, inspect};

// 通过 C 状态、字段标记及值分类区分三种存在性，不按空正文猜测。
// 步骤：查询版本 2 → 枚举字段及约束 → 分别处理缺失、null、值 → 归还持有并提交位图。
static dfy_v2_status DFY_V2_CALL inspect_presence(dfy_v2_query_interface_fn query, dfy_v2_handle structure,
    uint32_t* output, dfy_v2_error* error) {
    services api;
    dfy_v2_handle type = {0};
    dfy_v2_type_info info = {0};
    dfy_v2_status status;
    uint64_t i;
    uint32_t states = 0;
    if (output) *output = 0;
    if (error) error->length = 0;
    if (!query || !output) return DFY_V2_INVALID_ARGUMENT;

    api.refs = (const dfy_v2_reference_api*)service(query, DFY_V2_REFERENCE_INTERFACE, 1, sizeof(*api.refs));
    api.types = (const dfy_v2_type_api_v2*)service(query, DFY_V2_TYPE_INTERFACE, 2, sizeof(*api.types));
    api.values = (const dfy_v2_value_api_v2*)service(query, DFY_V2_VALUE_INTERFACE, 2, sizeof(*api.values));
    api.structures = (const dfy_v2_struct_api_v2*)service(query, DFY_V2_STRUCT_INTERFACE, 2, sizeof(*api.structures));
    if (!api.refs || !api.types || !api.values || !api.structures || !api.refs->release || !api.types->describe ||
        !api.types->field || !api.values->read || !api.structures->descriptor || !api.structures->field) return DFY_V2_UNSUPPORTED;

    status = api.structures->descriptor(structure, &type, 0); if (status) return status;
    status = api.types->describe(type, &info, 0);

    for (i = 0; !status && i < info.field_count; ++i) {
        dfy_v2_field_info_v2 field = {0};
        dfy_v2_handle owned = {0};
        dfy_v2_value_view value = {0};
        status = api.types->field(type, i, &field, 0);
        if (!status && (field.flags & ~(DFY_V2_FIELD_OPTIONAL | DFY_V2_FIELD_NULLABLE))) status = DFY_V2_UNSUPPORTED;
        if (!status) status = api.structures->field(structure, i, &owned, 0);
        if (status == DFY_V2_NOT_FOUND && (field.flags & DFY_V2_FIELD_OPTIONAL)) { states |= 2; status = DFY_V2_OK; }
        else if (!status) {
            status = api.values->read(owned, &value, 0);
            if (!status && value.kind == DFY_V2_VALUE_NULL) {
                if (!(field.flags & DFY_V2_FIELD_NULLABLE) || value.data.uint64 || value.data.bytes.length) status = DFY_V2_FAILED;
                else states |= 4;
            } else if (!status && value.kind == field.kind) states |= 1;
            else if (!status) status = DFY_V2_FAILED;
        }

        api.refs->release(owned, 0);
    }

    api.refs->release(type, 0);
    if (!status) *output = states;
    return status;
}
const dfy_test_presence_api dfy_test_presence = {{sizeof(dfy_test_presence_api), 1}, inspect_presence};
