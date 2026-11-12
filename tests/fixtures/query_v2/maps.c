#include "api.h"
#include <stddef.h>
#include <string.h>
_Static_assert(sizeof(dfy_v2_map_type_info) == 32 && offsetof(dfy_v2_map_type_info,max_key_bytes) == 16,"map type layout");
_Static_assert(sizeof(dfy_v2_map_info) == 40 && offsetof(dfy_v2_map_info,size) == 32,"map info layout");
_Static_assert(sizeof(dfy_v2_map_api) == 40 && offsetof(dfy_v2_map_api,find) == 32,"map table layout");
// 精确查询能力并校验共同前缀。
static const void* service(dfy_v2_query_interface_fn query,const char* name,uint32_t version,uint32_t size) {
    const dfy_v2_string_view id = {name,(uint64_t)strlen(name)};
    const void* table = 0;
    const dfy_v2_api_header* header;
    if (query(id,version,&table,0)) return 0;
    header = (const dfy_v2_api_header*)table;
    return header && header->struct_size == size && header->interface_version == version ? table : 0;
}
// 同时验收映射反射、按序读取及精确键查询，不预编译键集合。
// 步骤：查询服务 → 核对声明与实际约束 → 枚举并再次查键 → 归还全部持有。
static dfy_v2_status DFY_V2_CALL inspect(dfy_v2_query_interface_fn query,dfy_v2_handle structure,uint64_t index,
    dfy_test_map_counts* output,dfy_v2_error* error) {
    const dfy_v2_reference_api* refs;
    const dfy_v2_struct_api_v2* structures;
    const dfy_v2_value_api_v2* values;
    const dfy_v2_map_api* maps;
    dfy_test_map_counts count = {0};
    dfy_v2_handle type = {0},map = {0};
    dfy_v2_map_type_info declared = {0};
    dfy_v2_map_info actual = {0};
    dfy_v2_status status;
    uint64_t i;
    if (output) *output = count;
    if (error) error->length = 0;
    if (!query || !output) return DFY_V2_INVALID_ARGUMENT;

    refs = (const dfy_v2_reference_api*)service(query,DFY_V2_REFERENCE_INTERFACE,1,sizeof(*refs));
    structures = (const dfy_v2_struct_api_v2*)service(query,DFY_V2_STRUCT_INTERFACE,2,sizeof(*structures));
    values = (const dfy_v2_value_api_v2*)service(query,DFY_V2_VALUE_INTERFACE,2,sizeof(*values));
    maps = (const dfy_v2_map_api*)service(query,DFY_V2_MAP_INTERFACE,1,sizeof(*maps));
    if (!refs || !structures || !values || !maps || !refs->release || !structures->descriptor || !structures->field || !values->read ||
        !maps->field_type || !maps->describe || !maps->entry || !maps->find) return DFY_V2_UNSUPPORTED;

    status = structures->descriptor(structure,&type,0);
    if (!status) status = maps->field_type(type,index,&declared,0);
    if (!status) status = structures->field(structure,index,&map,0);
    if (!status) status = maps->describe(map,&actual,0);
    if (!status && (declared.value_kind < DFY_V2_VALUE_BOOL || declared.value_kind > DFY_V2_VALUE_BYTES ||
        (declared.value_flags & ~DFY_V2_ELEMENT_NULLABLE))) status = DFY_V2_UNSUPPORTED;
    if (!status && (declared.value_kind != actual.type.value_kind || declared.value_flags != actual.type.value_flags ||
        declared.max_entries != actual.type.max_entries || declared.max_key_bytes != actual.type.max_key_bytes ||
        declared.max_value_bytes != actual.type.max_value_bytes || actual.size > declared.max_entries)) status = DFY_V2_FAILED;

    for (i = 0; !status && i < actual.size; ++i) {
        dfy_v2_string_view key = {0};
        dfy_v2_handle item = {0},found = {0};
        dfy_v2_value_view value = {0},again = {0};
        status = maps->entry(map,i,&key,&item,0);
        if (!status) status = values->read(item,&value,0);
        if (!status) status = maps->find(map,key,&found,0);
        if (!status) status = values->read(found,&again,0);
        if (!status && value.kind != again.kind) status = DFY_V2_FAILED;
        if (!status && value.kind == DFY_V2_VALUE_UINT64 && value.data.uint64 != again.data.uint64) status = DFY_V2_FAILED;
        if (!status && value.kind == DFY_V2_VALUE_NULL) {
            if (!(declared.value_flags & DFY_V2_ELEMENT_NULLABLE)) status = DFY_V2_FAILED;
            else ++count.nulls;
        } else if (!status && value.kind != declared.value_kind) status = DFY_V2_FAILED;
        if (!status) { count.key_bytes += key.length; if (value.kind == DFY_V2_VALUE_UINT64) count.last_unsigned = value.data.uint64; }
        refs->release(found,0); refs->release(item,0);
    }

    refs->release(map,0); refs->release(type,0);
    if (!status) { count.size = actual.size; *output = count; }
    return status;
}
const dfy_test_maps_api dfy_test_maps = {{sizeof(dfy_test_maps_api),1},inspect};
