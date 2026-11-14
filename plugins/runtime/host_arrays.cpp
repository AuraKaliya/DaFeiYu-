#include "host_state.hpp"
#include <dafeiyu/array_value.hpp>

namespace dfy::plugins::detail {
namespace {
// 按精确版本复制数组约束，元素可空标记不混用字段可选标记。
dfy_v2_array_type_info describe_type(const runtime::ArrayDescriptor& type, uint32_t profile) {
    return {value_kind(type.element_kind(), profile), type.nullable() ? DFY_V2_ELEMENT_NULLABLE : 0,
        type.max_elements(), type.max_element_bytes()};
}
// 返回结构字段内的数组约束，失败清空输出。
dfy_v2_status field_info(dfy_v2_handle type, uint64_t index, dfy_v2_array_type_info* output, dfy_v2_error* error, uint32_t profile) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        const auto fields = object<runtime::StructDescriptor>(host, type).fields();
        if (index >= fields.size() || !fields[static_cast<std::size_t>(index)].array) return DFY_V2_INVALID_ARGUMENT;
        *output = describe_type(*fields[static_cast<std::size_t>(index)].array,profile);
        return DFY_V2_OK;
    });
}
// 读取数组信息副本，不复制元素或改变持有关系。
dfy_v2_status describe_value(dfy_v2_handle source, dfy_v2_array_info* output, dfy_v2_error* error, uint32_t profile) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        const auto& value = object<runtime::Value>(host, source).as_array();
        *output = {describe_type(*value.descriptor(),profile), value.size()};
        return DFY_V2_OK;
    });
}
// 通过父值别名保留元素，递归取得的数据仍承担原事件与结果预算。
// 步骤：清空输出 → 校验数组及索引 → 签发元素独立持有。
dfy_v2_status element_value(dfy_v2_handle source, uint64_t index, dfy_v2_handle* output, dfy_v2_error* error, uint32_t profile) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        const auto& parent = std::get<std::shared_ptr<const runtime::Value>>(host.references.get(native(source)).object);
        const auto& array = parent->as_array();
        (void)value_kind(array.descriptor()->element_kind(),profile);
        if (index >= array.size()) return DFY_V2_INVALID_ARGUMENT;
        *output = store_object(host, std::shared_ptr<const runtime::Value>(parent, &array.at(static_cast<std::size_t>(index))));
        return DFY_V2_OK;
    });
}
// 读取版本 1 的标量字段约束。
dfy_v2_status DFY_V2_CALL field_type(dfy_v2_handle type, uint64_t index, dfy_v2_array_type_info* output, dfy_v2_error* error) {
    return field_info(type,index,output,error,1);
}
// 读取版本 2 的完整元素分类。
dfy_v2_status DFY_V2_CALL field_type_v2(dfy_v2_handle type, uint64_t index, dfy_v2_array_type_info* output, dfy_v2_error* error) {
    return field_info(type,index,output,error,2);
}
// 保留版本 1 对组合元素声明的拒绝。
dfy_v2_status DFY_V2_CALL describe(dfy_v2_handle value, dfy_v2_array_info* output, dfy_v2_error* error) {
    return describe_value(value,output,error,1);
}
// 返回版本 2 的实际数量和完整分类。
dfy_v2_status DFY_V2_CALL describe_v2(dfy_v2_handle value, dfy_v2_array_info* output, dfy_v2_error* error) {
    return describe_value(value,output,error,2);
}
// 版本 1 仅签发标量声明中的元素，null 不绕过声明检查。
dfy_v2_status DFY_V2_CALL element(dfy_v2_handle value, uint64_t index, dfy_v2_handle* output, dfy_v2_error* error) {
    return element_value(value,index,output,error,1);
}
// 版本 2 签发任意已实现内容的独立元素引用。
dfy_v2_status DFY_V2_CALL element_v2(dfy_v2_handle value, uint64_t index, dfy_v2_handle* output, dfy_v2_error* error) {
    return element_value(value,index,output,error,2);
}
// 为实际数组的匿名元素描述签发独立元数据引用，不保留数组数据。
// 步骤：清空输出 → 校验数组值 → 保持描述并别名引用元素约束。
dfy_v2_status DFY_V2_CALL element_type(dfy_v2_handle source, dfy_v2_handle* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        const auto descriptor = object<runtime::Value>(host,source).as_array().descriptor();
        *output = store_object(host,std::shared_ptr<const runtime::FieldDescriptor>(descriptor,&descriptor->element().constraint()));
        return DFY_V2_OK;
    });
}
}
// 提供精确版本的数组反射和元素访问。
const dfy_v2_array_api& array_api() {
    static const dfy_v2_array_api api = {{sizeof(dfy_v2_array_api), 1}, field_type, describe, element};
    return api;
}
// 返回独立版本 2 的组合数组访问表，不更改版本 1 布局。
const dfy_v2_array_api_v2& array_api_v2() {
    static const dfy_v2_array_api_v2 api = {{sizeof(dfy_v2_array_api_v2),2},field_type_v2,describe_v2,element_v2,element_type};
    return api;
}
}
