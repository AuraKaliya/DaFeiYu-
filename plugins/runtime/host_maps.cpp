#include "host_state.hpp"
#include <dafeiyu/map_value.hpp>

namespace dfy::plugins::detail {
namespace {
// 显式复制映射约束，不借用键或数据。
dfy_v2_map_type_info describe_type(const runtime::MapDescriptor& type) {
    return {value_kind(type.value_kind(),2), type.nullable() ? DFY_V2_ELEMENT_NULLABLE : 0,
        type.max_entries(), type.max_key_bytes(), type.max_value_bytes()};
}
// 读取结构字段的映射约束，错误输出清空。
dfy_v2_status DFY_V2_CALL field_type(dfy_v2_handle source, uint64_t index, dfy_v2_map_type_info* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error,[&](auto& host) {
        const auto fields = object<runtime::StructDescriptor>(host,source).fields();
        if (index >= fields.size() || !fields[static_cast<std::size_t>(index)].map) return DFY_V2_INVALID_ARGUMENT;
        *output = describe_type(*fields[static_cast<std::size_t>(index)].map);
        return DFY_V2_OK;
    });
}
// 返回实际映射约束和数量，不复制任何条目。
dfy_v2_status DFY_V2_CALL describe(dfy_v2_handle source, dfy_v2_map_info* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error,[&](auto& host) {
        const auto& map = object<runtime::Value>(host,source).as_map();
        *output = {describe_type(*map.descriptor()),map.size()};
        return DFY_V2_OK;
    });
}
// 在值引用成功签发后才交付键借用，失败不留下部分输出。
// 步骤：清空输出 → 核对映射和索引 → 保留值别名 → 返回键借用。
dfy_v2_status DFY_V2_CALL entry(dfy_v2_handle source, uint64_t index, dfy_v2_string_view* key, dfy_v2_handle* output, dfy_v2_error* error) {
    if (key) *key = {};
    if (output) *output = {};
    if (!key || !output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error,[&](auto& host) {
        const auto& parent = std::get<std::shared_ptr<const runtime::Value>>(host.references.get(native(source)).object);
        const auto& map = parent->as_map();
        if (index >= map.size()) return DFY_V2_INVALID_ARGUMENT;
        const auto& item = map.at(static_cast<std::size_t>(index));
        *output = store_object(host,std::shared_ptr<const runtime::Value>(parent,&item.value));
        *key = {item.key.data(),item.key.size()};
        return DFY_V2_OK;
    });
}
// 按完整键查找，空键可用，存在的 null 不当作未知键。
// 步骤：清空输出并校验视图 → 查询实际键 → 签发独立值持有或报告未知。
dfy_v2_status DFY_V2_CALL find(dfy_v2_handle source, dfy_v2_string_view key, dfy_v2_handle* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (error) error->length = 0;
    if (!output || (!key.data && key.length)) return DFY_V2_INVALID_ARGUMENT;
    return boundary(error,[&](auto& host) {
        const auto& parent = std::get<std::shared_ptr<const runtime::Value>>(host.references.get(native(source)).object);
        const auto* value = parent->as_map().find({key.data ? key.data : "",static_cast<std::size_t>(key.length)});
        if (!value) return DFY_V2_NOT_FOUND;
        *output = store_object(host,std::shared_ptr<const runtime::Value>(parent,value));
        return DFY_V2_OK;
    });
}
}
// 提供精确版本的字符串键映射服务。
const dfy_v2_map_api& map_api() {
    static const dfy_v2_map_api api = {{sizeof(dfy_v2_map_api),1},field_type,describe,entry,find};
    return api;
}
}
