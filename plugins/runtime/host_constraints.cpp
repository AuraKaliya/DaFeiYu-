#include "host_state.hpp"
#include <dafeiyu/array_value.hpp>
#include <dafeiyu/map_value.hpp>

namespace dfy::plugins::detail {
namespace {
// 从结构描述保持字段约束，别名只延长描述寿命，不保持数据载荷。
// 步骤：清空输出 → 核对描述、索引及已实现种类 → 签发约束引用。
dfy_v2_status DFY_V2_CALL field(dfy_v2_handle type, uint64_t index, dfy_v2_handle* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        const auto& parent = std::get<std::shared_ptr<const runtime::StructDescriptor>>(host.references.get(native(type)).object);
        if (index >= parent->fields().size()) return DFY_V2_INVALID_ARGUMENT;
        const auto& selected = parent->fields()[static_cast<std::size_t>(index)];
        (void)value_kind(selected.kind,2);
        *output = store_object(host,std::shared_ptr<const runtime::FieldDescriptor>(parent,&selected));
        return DFY_V2_OK;
    });
}
// 返回独立约束的只读元数据，保持现有版本 2 的分类与标志规则。
// 步骤：清空输出 → 校验约束引用 → 复制标志与字节约束并借用名称。
dfy_v2_status DFY_V2_CALL describe(dfy_v2_handle source, dfy_v2_field_info_v2* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        const auto& field = object<runtime::FieldDescriptor>(host,source);
        *output = {{field.name.data(),field.name.size()},value_kind(field.kind,2),
            (field.optional ? DFY_V2_FIELD_OPTIONAL : 0) | (field.nullable ? DFY_V2_FIELD_NULLABLE : 0),field.max_bytes};
        return DFY_V2_OK;
    });
}
// 保持约束引用的命名子类型，不把描述类别隐式转为另一种类型。
// 步骤：清空输出 → 校验约束 → 按已实现种类签发描述。
dfy_v2_status DFY_V2_CALL named_type(dfy_v2_handle source, dfy_v2_handle* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        const auto& field = object<runtime::FieldDescriptor>(host,source);
        if (field.structure) *output = store_object(host,field.structure);
        else if (field.enumeration) *output = store_object(host,field.enumeration);
        else return DFY_V2_INVALID_ARGUMENT;
        return DFY_V2_OK;
    });
}
// 读取数组内容约束副本，失败保持输出全零。
// 步骤：清空输出 → 核对数组描述 → 复制元素与容量约束。
dfy_v2_status DFY_V2_CALL array(dfy_v2_handle source, dfy_v2_array_type_info* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        const auto& field = object<runtime::FieldDescriptor>(host,source);
        if (!field.array) return DFY_V2_INVALID_ARGUMENT;
        const auto& type = *field.array;
        *output = {value_kind(type.element_kind(),2),type.nullable() ? DFY_V2_ELEMENT_NULLABLE : 0,type.max_elements(),type.max_element_bytes()};
        return DFY_V2_OK;
    });
}
// 读取字符串键映射的内容约束副本，不签发数据引用。
// 步骤：清空输出 → 核对映射描述 → 复制键、值及容量约束。
dfy_v2_status DFY_V2_CALL map(dfy_v2_handle source, dfy_v2_map_type_info* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        const auto& field = object<runtime::FieldDescriptor>(host,source);
        if (!field.map) return DFY_V2_INVALID_ARGUMENT;
        const auto& type = *field.map;
        *output = {value_kind(type.value_kind(),2),type.nullable() ? DFY_V2_ELEMENT_NULLABLE : 0,type.max_entries(),type.max_key_bytes(),type.max_value_bytes()};
        return DFY_V2_OK;
    });
}
}
// 提供独立字段约束反射，为后续联合和组合容器复用相同读取规则。
const dfy_v2_constraint_api& constraint_api() {
    static const dfy_v2_constraint_api api = {{sizeof(dfy_v2_constraint_api),1},field,describe,named_type,array,map};
    return api;
}
}
