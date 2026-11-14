#include "api.h"
#include <string.h>

// 精确查询能力与布局，不依赖函数表的隐式大小兼容。
// 步骤：查询目标版本 → 核对公共前缀 → 返回只读表。
static const void* service(dfy_v2_query_interface_fn query, const char* name, uint32_t version, uint32_t size) {
    const dfy_v2_string_view id = {name,(uint64_t)strlen(name)};
    const void* table = 0; const dfy_v2_api_header* header;
    if (query(id,version,&table,0)) return 0;
    header = (const dfy_v2_api_header*)table;
    return header && header->struct_size == size && header->interface_version == version ? table : 0;
}
// 在纯 C 中反射完整映射，逐项以完整键再次查找并保留非 null 值。
// 步骤：协商版本 → 比较声明与实际约束 → 有界枚举和精确查键 → 清理并转交数据持有。
static dfy_v2_status DFY_V2_CALL inspect(dfy_v2_query_interface_fn query, dfy_v2_handle structure, uint64_t index,
    uint64_t budget, dfy_test_composite_map_counts* output, dfy_v2_handle* held, dfy_v2_error* error) {
    const dfy_v2_reference_api* refs;
    const dfy_v2_struct_api_v2* structures;
    const dfy_v2_value_api_v2* values;
    const dfy_v2_map_api_v2* maps;
    const dfy_v2_constraint_api_v2* constraints;
    dfy_v2_handle parent = {0}, map = {0}, type = {0}, named = {0}, last = {0}, empty = {0};
    dfy_v2_map_type_info declared = {0}; dfy_v2_map_info info = {0}; dfy_v2_field_info_v2 schema = {0};
    dfy_test_composite_map_counts count = {0}; uint64_t i; dfy_v2_status status;
    if (output) *output = count;
    if (held) *held = empty;
    if (error) error->length = 0;
    if (!query || !output || !held) return DFY_V2_INVALID_ARGUMENT;

    refs = (const dfy_v2_reference_api*)service(query,DFY_V2_REFERENCE_INTERFACE,1,sizeof(*refs));
    structures = (const dfy_v2_struct_api_v2*)service(query,DFY_V2_STRUCT_INTERFACE,2,sizeof(*structures));
    values = (const dfy_v2_value_api_v2*)service(query,DFY_V2_VALUE_INTERFACE,2,sizeof(*values));
    maps = (const dfy_v2_map_api_v2*)service(query,DFY_V2_MAP_INTERFACE,2,sizeof(*maps));
    constraints = (const dfy_v2_constraint_api_v2*)service(query,DFY_V2_CONSTRAINT_INTERFACE,2,sizeof(*constraints));
    if (!refs || !structures || !values || !maps || !constraints || !refs->release || !structures->descriptor || !structures->field || !values->read ||
        !maps->field_type || !maps->describe || !maps->entry || !maps->find || !maps->value_type ||
        !constraints->describe || !constraints->named_type || !constraints->array || !constraints->map) return DFY_V2_UNSUPPORTED;
    status = structures->descriptor(structure,&parent,0);
    if (!status) status = maps->field_type(parent,index,&declared,0);
    if (!status) status = structures->field(structure,index,&map,0);
    if (!status) status = maps->describe(map,&info,0);
    if (!status && (info.size > budget || info.size > info.type.max_entries)) status = DFY_V2_RESOURCE_EXHAUSTED;
    if (!status && (declared.value_kind != info.type.value_kind || declared.value_flags != info.type.value_flags || declared.max_entries != info.type.max_entries ||
        declared.max_key_bytes != info.type.max_key_bytes || declared.max_value_bytes != info.type.max_value_bytes)) status = DFY_V2_FAILED;
    if (!status) status = maps->value_type(map,&type,0);
    if (!status) status = constraints->describe(type,&schema,0);
    if (!status && (schema.kind < DFY_V2_VALUE_BOOL || schema.kind > DFY_V2_VALUE_UNION || schema.kind == DFY_V2_VALUE_NULL ||
        (schema.flags & ~DFY_V2_FIELD_NULLABLE))) status = DFY_V2_UNSUPPORTED;
    if (!status && (schema.name.length || schema.kind != info.type.value_kind || schema.max_bytes != info.type.max_value_bytes ||
        info.type.value_flags != ((schema.flags & DFY_V2_FIELD_NULLABLE) ? DFY_V2_ELEMENT_NULLABLE : 0))) status = DFY_V2_FAILED;
    if (!status && (schema.kind == DFY_V2_VALUE_STRUCTURE || schema.kind == DFY_V2_VALUE_ENUM || schema.kind == DFY_V2_VALUE_UNION))
        status = constraints->named_type(type,&named,0);
    if (!status && schema.kind == DFY_V2_VALUE_ARRAY) { dfy_v2_array_type_info nested = {0}; status = constraints->array(type,&nested,0); }
    if (!status && schema.kind == DFY_V2_VALUE_MAP) { dfy_v2_map_type_info nested = {0}; status = constraints->map(type,&nested,0); }
    refs->release(named,0);

    for (i = 0; !status && i < info.size; ++i) {
        dfy_v2_handle element = {0}, found = {0}; dfy_v2_string_view key = {0}; dfy_v2_value_view value = {0}, repeated = {0};
        status = maps->entry(map,i,&key,&element,0);
        if (!status && ((key.length && !key.data) || key.length > info.type.max_key_bytes)) status = DFY_V2_FAILED;
        if (!status) status = maps->find(map,key,&found,0);
        if (!status) status = values->read(element,&value,0);
        if (!status) status = values->read(found,&repeated,0);
        if (!status && (value.reserved || repeated.reserved || value.kind != repeated.kind)) status = DFY_V2_FAILED;
        if (!status && value.kind == DFY_V2_VALUE_NULL) {
            if (!(schema.flags & DFY_V2_FIELD_NULLABLE)) status = DFY_V2_FAILED;
            else ++count.nulls;
        } else if (!status && value.kind != schema.kind) status = DFY_V2_FAILED;
        else if (!status) { refs->release(last,0); last = element; element = empty; }
        if (!status) count.key_bytes += key.length;
        refs->release(found,0); refs->release(element,0);
    }

    refs->release(type,0); refs->release(map,0); refs->release(parent,0);
    if (!status) { count.size = info.size; count.kind = schema.kind; *output = count; *held = last; }
    else refs->release(last,0);
    return status;
}
const dfy_test_composite_maps_api dfy_test_composite_maps = {{sizeof(dfy_test_composite_maps_api),1},inspect};
