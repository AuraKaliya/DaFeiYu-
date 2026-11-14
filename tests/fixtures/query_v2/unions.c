#include "api.h"
#include <string.h>

// 精确查询能力，不将其他版本或布局当作兼容函数表。
// 步骤：查询目标版本 → 核对公共前缀 → 返回匹配结果。
static const void* service(dfy_v2_query_interface_fn query, const char* name, uint32_t version, uint32_t size) {
    const dfy_v2_string_view id = {name,(uint64_t)strlen(name)};
    const void* table = 0;
    const dfy_v2_api_header* header;
    if (query(id,version,&table,0)) return 0;
    header = (const dfy_v2_api_header*)table;
    return header && header->struct_size == size && header->interface_version == version ? table : 0;
}
// 在纯 C 中验证复杂分支具有对应真实描述，不把联合伪装为结构。
// 步骤：读取容器约束或命名类型 → 查询相应元数据 → 核对实际种类 → 归还描述引用。
static dfy_v2_status reflect(dfy_v2_query_interface_fn query, const dfy_v2_constraint_api* constraints,
    const dfy_v2_reference_api* refs, dfy_v2_handle source, uint32_t kind) {
    dfy_v2_handle named = {0}; dfy_v2_status status;
    if (kind == DFY_V2_VALUE_ARRAY) {
        dfy_v2_array_type_info info = {0};
        return constraints->array(source,&info,0);
    }
    if (kind == DFY_V2_VALUE_MAP) {
        dfy_v2_map_type_info info = {0};
        return constraints->map(source,&info,0);
    }
    if (kind <= DFY_V2_VALUE_BYTES) return DFY_V2_OK;
    status = constraints->named_type(source,&named,0);
    if (!status && kind == DFY_V2_VALUE_STRUCTURE) {
        const dfy_v2_type_api* api = (const dfy_v2_type_api*)service(query,DFY_V2_TYPE_INTERFACE,1,sizeof(*api));
        dfy_v2_type_info info = {0};
        status = api && api->describe ? api->describe(named,&info,0) : DFY_V2_UNSUPPORTED;
        if (!status && (!info.name.data || !info.name.length || !info.contract_version || info.reserved)) status = DFY_V2_FAILED;
    } else if (!status && kind == DFY_V2_VALUE_ENUM) {
        const dfy_v2_enum_api* api = (const dfy_v2_enum_api*)service(query,DFY_V2_ENUM_INTERFACE,1,sizeof(*api));
        dfy_v2_enum_info info = {0};
        status = api && api->describe ? api->describe(named,&info,0) : DFY_V2_UNSUPPORTED;
        if (!status && (!info.name.data || !info.name.length || !info.contract_version || !info.member_count)) status = DFY_V2_FAILED;
    } else if (!status && kind == DFY_V2_VALUE_UNION) {
        const dfy_v2_union_api* api = (const dfy_v2_union_api*)service(query,DFY_V2_UNION_INTERFACE,1,sizeof(*api));
        dfy_v2_union_info info = {0};
        status = api && api->describe ? api->describe(named,&info,0) : DFY_V2_UNSUPPORTED;
        if (!status && (!info.name.data || !info.name.length || !info.contract_version || !info.branch_count || info.reserved)) status = DFY_V2_FAILED;
    } else if (!status) status = DFY_V2_UNSUPPORTED;
    refs->release(named,0);
    return status;
}
// 有界反射未知联合的全部分支，转交实际内容的一份独立持有。
// 步骤：协商能力 → 读取联合选择 → 有界核对全部分支约束 → 匹配实际内容 → 清理并提交结果。
static dfy_v2_status DFY_V2_CALL inspect(dfy_v2_query_interface_fn query, dfy_v2_handle structure, uint64_t index,
    uint64_t branch_budget, dfy_test_union_counts* output, dfy_v2_handle* held, dfy_v2_error* error) {
    const dfy_v2_reference_api* refs;
    const dfy_v2_struct_api_v2* structures;
    const dfy_v2_value_api_v2* values;
    const dfy_v2_union_api* unions;
    const dfy_v2_constraint_api* constraints;
    dfy_v2_handle value = {0}, type = {0}, contents = {0}, empty = {0};
    dfy_v2_union_info info = {0}; dfy_v2_union_selection selection = {0}; dfy_v2_value_view view = {0};
    dfy_test_union_counts count = {0};
    uint32_t expected_kind = 0, flags = 0; uint64_t i; dfy_v2_status status;
    if (output) *output = count;
    if (held) *held = empty;
    if (error) error->length = 0;
    if (!query || !output || !held) return DFY_V2_INVALID_ARGUMENT;
    refs = (const dfy_v2_reference_api*)service(query,DFY_V2_REFERENCE_INTERFACE,1,sizeof(*refs));
    structures = (const dfy_v2_struct_api_v2*)service(query,DFY_V2_STRUCT_INTERFACE,2,sizeof(*structures));
    values = (const dfy_v2_value_api_v2*)service(query,DFY_V2_VALUE_INTERFACE,2,sizeof(*values));
    unions = (const dfy_v2_union_api*)service(query,DFY_V2_UNION_INTERFACE,1,sizeof(*unions));
    constraints = (const dfy_v2_constraint_api*)service(query,DFY_V2_CONSTRAINT_INTERFACE,1,sizeof(*constraints));
    if (!refs || !structures || !values || !unions || !constraints || !refs->release || !structures->field || !values->read ||
        !unions->describe || !unions->branch || !unions->descriptor || !unions->read || !unions->content ||
        !constraints->describe || !constraints->named_type || !constraints->array || !constraints->map) return DFY_V2_UNSUPPORTED;
    status = structures->field(structure,index,&value,0);
    if (!status) status = unions->descriptor(value,&type,0);
    if (!status) status = unions->describe(type,&info,0);
    if (!status && (!info.branch_count || info.reserved)) status = DFY_V2_FAILED;
    if (!status && info.branch_count > branch_budget) status = DFY_V2_RESOURCE_EXHAUSTED;
    if (!status) status = unions->read(value,&selection,0);
    if (!status && selection.index >= info.branch_count) status = DFY_V2_FAILED;

    for (i = 0; !status && i < info.branch_count; ++i) {
        dfy_v2_handle branch = {0}; dfy_v2_field_info_v2 schema = {0}; uint64_t tag = 0;
        status = unions->branch(type,i,&tag,&branch,0);
        if (!status) status = constraints->describe(branch,&schema,0);
        if (!status && (!schema.name.data || !schema.name.length || (schema.flags & ~DFY_V2_FIELD_NULLABLE) ||
            schema.kind < DFY_V2_VALUE_BOOL || schema.kind > DFY_V2_VALUE_UNION || schema.kind == DFY_V2_VALUE_NULL)) status = DFY_V2_UNSUPPORTED;
        if (!status) status = reflect(query,constraints,refs,branch,schema.kind);
        if (!status) count.kinds |= UINT32_C(1) << schema.kind;
        if (!status && i == selection.index) {
            if (tag != selection.tag) status = DFY_V2_FAILED;
            expected_kind = schema.kind; flags = schema.flags;
        }
        refs->release(branch,0);
    }

    if (!status) status = unions->content(value,&contents,0);
    if (!status) status = values->read(contents,&view,0);
    if (!status && view.kind != expected_kind && !(view.kind == DFY_V2_VALUE_NULL && (flags & DFY_V2_FIELD_NULLABLE))) status = DFY_V2_FAILED;
    refs->release(type,0); refs->release(value,0);
    if (!status) { count.branches = info.branch_count; count.tag = selection.tag; count.content_kind = view.kind; *output = count; *held = contents; }
    else refs->release(contents,0);
    return status;
}
const dfy_test_unions_api dfy_test_unions = {{sizeof(dfy_test_unions_api),1},inspect};
