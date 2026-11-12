#include "host_state.hpp"
#include <dafeiyu/array_value.hpp>

namespace dfy::plugins::detail {
namespace {
// 显式复制标量数组约束，元素可空标记不混用字段可选标记。
dfy_v2_array_type_info describe_type(const runtime::ArrayDescriptor& type) {
    return {value_kind(type.element_kind(), 2), type.nullable() ? DFY_V2_ELEMENT_NULLABLE : 0,
        type.max_elements(), type.max_element_bytes()};
}
// 返回结构字段内的数组约束，失败清空输出。
dfy_v2_status DFY_V2_CALL field_type(dfy_v2_handle type, uint64_t index, dfy_v2_array_type_info* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        const auto fields = object<runtime::StructDescriptor>(host, type).fields();
        if (index >= fields.size() || !fields[static_cast<std::size_t>(index)].array) return DFY_V2_INVALID_ARGUMENT;
        *output = describe_type(*fields[static_cast<std::size_t>(index)].array);
        return DFY_V2_OK;
    });
}
// 读取数组信息副本，不复制元素或改变持有关系。
dfy_v2_status DFY_V2_CALL describe(dfy_v2_handle source, dfy_v2_array_info* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        const auto& value = object<runtime::Value>(host, source).as_array();
        *output = {describe_type(*value.descriptor()), value.size()};
        return DFY_V2_OK;
    });
}
// 通过父值别名保留元素，递归取得的数据仍承担原事件与结果预算。
// 步骤：清空输出 → 校验数组及索引 → 签发元素独立持有。
dfy_v2_status DFY_V2_CALL element(dfy_v2_handle source, uint64_t index, dfy_v2_handle* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        const auto& parent = std::get<std::shared_ptr<const runtime::Value>>(host.references.get(native(source)).object);
        const auto& array = parent->as_array();
        if (index >= array.size()) return DFY_V2_INVALID_ARGUMENT;
        *output = store_object(host, std::shared_ptr<const runtime::Value>(parent, &array.at(static_cast<std::size_t>(index))));
        return DFY_V2_OK;
    });
}
}
// 提供精确版本的数组反射和元素访问。
const dfy_v2_array_api& array_api() {
    static const dfy_v2_array_api api = {{sizeof(dfy_v2_array_api), 1}, field_type, describe, element};
    return api;
}
}
