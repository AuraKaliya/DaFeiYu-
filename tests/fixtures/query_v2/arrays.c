#include "api.h"
#include <stddef.h>
#include <string.h>
_Static_assert(sizeof(dfy_v2_array_type_info) == 24 && offsetof(dfy_v2_array_type_info, max_elements) == 8, "array type layout");
_Static_assert(sizeof(dfy_v2_array_info) == 32 && offsetof(dfy_v2_array_info, size) == 24, "array info layout");
_Static_assert(sizeof(dfy_v2_array_api) == 32 && offsetof(dfy_v2_array_api, element) == 24, "array table layout");
// 精确查询能力表，不把更大的表或其他版本当作兼容布局。
static const void* service(dfy_v2_query_interface_fn query, const char* name, uint32_t version, uint32_t size) {
    const dfy_v2_string_view id = {name,(uint64_t)strlen(name)};
    const void* table = 0;
    const dfy_v2_api_header* header;
    if (query(id,version,&table,0)) return 0;
    header = (const dfy_v2_api_header*)table;
    return header && header->struct_size == size && header->interface_version == version ? table : 0;
}
// 在纯 C 中核对数组声明与实际元素，包含 null 的槽位仍计入实际数量。
// 步骤：查询能力 → 取得声明与数组 → 逐项读取并归还 → 提交统计。
static dfy_v2_status DFY_V2_CALL inspect(dfy_v2_query_interface_fn query, dfy_v2_handle structure, uint64_t index,
    dfy_test_array_counts* output, dfy_v2_error* error) {
    const dfy_v2_reference_api* refs;
    const dfy_v2_struct_api_v2* structures;
    const dfy_v2_value_api_v2* values;
    const dfy_v2_array_api* arrays;
    dfy_test_array_counts count = {0};
    dfy_v2_handle type = {0}, array = {0};
    dfy_v2_array_type_info declared = {0};
    dfy_v2_array_info actual = {0};
    dfy_v2_status status;
    uint64_t i;
    if (output) *output = count;
    if (error) error->length = 0;
    if (!query || !output) return DFY_V2_INVALID_ARGUMENT;
    refs = (const dfy_v2_reference_api*)service(query,DFY_V2_REFERENCE_INTERFACE,1,sizeof(*refs));
    structures = (const dfy_v2_struct_api_v2*)service(query,DFY_V2_STRUCT_INTERFACE,2,sizeof(*structures));
    values = (const dfy_v2_value_api_v2*)service(query,DFY_V2_VALUE_INTERFACE,2,sizeof(*values));
    arrays = (const dfy_v2_array_api*)service(query,DFY_V2_ARRAY_INTERFACE,1,sizeof(*arrays));
    if (!refs || !structures || !values || !arrays || !refs->release || !structures->descriptor || !structures->field ||
        !values->read || !arrays->field_type || !arrays->describe || !arrays->element) return DFY_V2_UNSUPPORTED;
    status = structures->descriptor(structure,&type,0);
    if (!status) status = arrays->field_type(type,index,&declared,0);
    if (!status) status = structures->field(structure,index,&array,0);
    if (!status) status = arrays->describe(array,&actual,0);
    if (!status && (actual.type.element_kind < DFY_V2_VALUE_BOOL || actual.type.element_kind > DFY_V2_VALUE_BYTES ||
        (actual.type.element_flags & ~DFY_V2_ELEMENT_NULLABLE))) status = DFY_V2_UNSUPPORTED;
    if (!status && actual.size > actual.type.max_elements) status = DFY_V2_FAILED;
    if (!status && (declared.element_kind != actual.type.element_kind || declared.element_flags != actual.type.element_flags ||
        declared.max_elements != actual.type.max_elements || declared.max_element_bytes != actual.type.max_element_bytes)) status = DFY_V2_FAILED;
    for (i = 0; !status && i < actual.size; ++i) {
        dfy_v2_handle element = {0};
        dfy_v2_value_view value = {0};
        status = arrays->element(array,i,&element,0);
        if (!status) status = values->read(element,&value,0);
        if (!status && value.kind == DFY_V2_VALUE_NULL) {
            if (!(actual.type.element_flags & DFY_V2_ELEMENT_NULLABLE)) status = DFY_V2_FAILED;
            else ++count.nulls;
        } else if (!status && value.kind != actual.type.element_kind) status = DFY_V2_FAILED;
        if (!status && value.kind == DFY_V2_VALUE_UINT64) count.last_unsigned = value.data.uint64;
        refs->release(element,0);
    }
    refs->release(array,0); refs->release(type,0);
    if (!status) { count.size = actual.size; *output = count; }
    return status;
}
const dfy_test_arrays_api dfy_test_arrays = {{sizeof(dfy_test_arrays_api),1},inspect};
