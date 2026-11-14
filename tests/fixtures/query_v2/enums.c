#include "api.h"
#include <string.h>

// 精确查询能力并核对布局，不保存调用上下文。
// 步骤：查询指定版本 → 校验公共前缀 → 返回匹配的只读表。
static const void* service(dfy_v2_query_interface_fn query, const char* name, uint32_t version, uint32_t size) {
    const dfy_v2_string_view id = {name,(uint64_t)strlen(name)};
    const void* table = 0;
    const dfy_v2_api_header* header;
    if (query(id,version,&table,0)) return 0;
    header = (const dfy_v2_api_header*)table;
    return header && header->struct_size == size && header->interface_version == version ? table : 0;
}
// 比较带种类整数，不按字节比较联合的未使用成员。
static int same_number(const dfy_v2_value_view* a, const dfy_v2_value_view* b) {
    if (a->kind != b->kind || a->reserved || b->reserved) return 0;
    if (a->kind == DFY_V2_VALUE_INT64) return a->data.int64 == b->data.int64;
    return a->kind == DFY_V2_VALUE_UINT64 && a->data.uint64 == b->data.uint64;
}
// 在明确成员预算内消费未知枚举，并把字段持有转交给调用方。
// 步骤：协商能力 → 读取声明及实际描述 → 有界枚举并核对选择 → 归还中间引用并提交输出。
static dfy_v2_status DFY_V2_CALL inspect(dfy_v2_query_interface_fn query, dfy_v2_handle structure, uint64_t index,
    uint64_t member_budget, dfy_test_enum_counts* output, dfy_v2_handle* held, dfy_v2_error* error) {
    const dfy_v2_reference_api* refs;
    const dfy_v2_struct_api_v2* structures;
    const dfy_v2_enum_api* enums;
    dfy_v2_handle type = {0}, declared = {0}, value = {0}, actual = {0}, empty = {0};
    dfy_v2_enum_info expected = {0}, info = {0};
    dfy_test_enum_counts count = {0};
    dfy_v2_status status;
    uint64_t i;
    if (output) *output = count;
    if (held) *held = empty;
    if (error) error->length = 0;
    if (!query || !output || !held) return DFY_V2_INVALID_ARGUMENT;

    refs = (const dfy_v2_reference_api*)service(query,DFY_V2_REFERENCE_INTERFACE,1,sizeof(*refs));
    structures = (const dfy_v2_struct_api_v2*)service(query,DFY_V2_STRUCT_INTERFACE,2,sizeof(*structures));
    enums = (const dfy_v2_enum_api*)service(query,DFY_V2_ENUM_INTERFACE,1,sizeof(*enums));
    if (!refs || !structures || !enums || !refs->release || !structures->descriptor || !structures->field ||
        !enums->field_type || !enums->describe || !enums->member || !enums->descriptor || !enums->read) return DFY_V2_UNSUPPORTED;
    status = structures->descriptor(structure,&type,0);
    if (!status) status = enums->field_type(type,index,&declared,0);
    if (!status) status = structures->field(structure,index,&value,0);
    if (!status) status = enums->descriptor(value,&actual,0);
    if (!status) status = enums->describe(declared,&expected,0);
    if (!status) status = enums->describe(actual,&info,0);
    if (!status && (!info.name.data || !info.name.length || !expected.name.data || info.name.length != expected.name.length ||
        memcmp(info.name.data,expected.name.data,(size_t)info.name.length) || info.contract_version != expected.contract_version ||
        info.underlying_kind != expected.underlying_kind || info.member_count != expected.member_count || !info.member_count)) status = DFY_V2_FAILED;
    if (!status && info.member_count > member_budget) status = DFY_V2_RESOURCE_EXHAUSTED;
    if (!status) status = enums->read(value,&count.selection,0);
    if (!status && (count.selection.index >= info.member_count || count.selection.number.kind != info.underlying_kind)) status = DFY_V2_FAILED;

    for (i = 0; !status && i < info.member_count; ++i) {
        dfy_v2_enum_member a = {0}, b = {0};
        status = enums->member(declared,i,&a,0);
        if (!status) status = enums->member(actual,i,&b,0);
        if (!status && (!a.name.data || !a.name.length || !b.name.data || a.name.length != b.name.length ||
            memcmp(a.name.data,b.name.data,(size_t)a.name.length) || !same_number(&a.number,&b.number))) status = DFY_V2_FAILED;
        if (!status && i == count.selection.index && !same_number(&a.number,&count.selection.number)) status = DFY_V2_FAILED;
    }

    refs->release(actual,0); refs->release(declared,0); refs->release(type,0);
    if (!status) { count.members = info.member_count; *output = count; *held = value; }
    else refs->release(value,0);
    return status;
}
const dfy_test_enums_api dfy_test_enums = {{sizeof(dfy_test_enums_api),1},inspect};
