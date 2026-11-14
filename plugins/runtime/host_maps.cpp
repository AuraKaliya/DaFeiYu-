#include "host_state.hpp"
#include <dafeiyu/map_value.hpp>

namespace dfy::plugins::detail {
namespace {
// 按精确版本复制映射约束，不借用键或数据。
dfy_v2_map_type_info describe_type(const runtime::MapDescriptor& type, uint32_t profile) {
    return {value_kind(type.value_kind(),profile), type.nullable() ? DFY_V2_ELEMENT_NULLABLE : 0,
        type.max_entries(), type.max_key_bytes(), type.max_value_bytes()};
}
// 读取结构字段的映射约束，错误输出清空。
dfy_v2_status field_info(dfy_v2_handle source, uint64_t index, dfy_v2_map_type_info* output, dfy_v2_error* error, uint32_t profile) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error,[&](auto& host) {
        const auto fields = object<runtime::StructDescriptor>(host,source).fields();
        if (index >= fields.size() || !fields[static_cast<std::size_t>(index)].map) return DFY_V2_INVALID_ARGUMENT;
        *output = describe_type(*fields[static_cast<std::size_t>(index)].map,profile);
        return DFY_V2_OK;
    });
}
// 返回实际映射约束和数量，不复制任何条目。
dfy_v2_status describe_value(dfy_v2_handle source, dfy_v2_map_info* output, dfy_v2_error* error, uint32_t profile) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error,[&](auto& host) {
        const auto& map = object<runtime::Value>(host,source).as_map();
        *output = {describe_type(*map.descriptor(),profile),map.size()};
        return DFY_V2_OK;
    });
}
// 在值引用成功签发后才交付键借用，失败不留下部分输出。
// 步骤：清空输出 → 核对映射和索引 → 保留值别名 → 返回键借用。
dfy_v2_status entry_value(dfy_v2_handle source, uint64_t index, dfy_v2_string_view* key, dfy_v2_handle* output, dfy_v2_error* error, uint32_t profile) {
    if (key) *key = {};
    if (output) *output = {};
    if (!key || !output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error,[&](auto& host) {
        const auto& parent = std::get<std::shared_ptr<const runtime::Value>>(host.references.get(native(source)).object);
        const auto& map = parent->as_map();
        (void)value_kind(map.descriptor()->value_kind(),profile);
        if (index >= map.size()) return DFY_V2_INVALID_ARGUMENT;
        const auto& item = map.at(static_cast<std::size_t>(index));
        *output = store_object(host,std::shared_ptr<const runtime::Value>(parent,&item.value));
        *key = {item.key.data(),item.key.size()};
        return DFY_V2_OK;
    });
}
// 按完整键查找，空键可用，存在的 null 不当作未知键。
// 步骤：清空输出并校验视图 → 查询实际键 → 签发独立值持有或报告未知。
dfy_v2_status find_value(dfy_v2_handle source, dfy_v2_string_view key, dfy_v2_handle* output, dfy_v2_error* error, uint32_t profile) {
    if (output) *output = {};
    if (error) error->length = 0;
    if (!output || (!key.data && key.length)) return DFY_V2_INVALID_ARGUMENT;
    return boundary(error,[&](auto& host) {
        const auto& parent = std::get<std::shared_ptr<const runtime::Value>>(host.references.get(native(source)).object);
        const auto& map = parent->as_map();
        (void)value_kind(map.descriptor()->value_kind(),profile);
        const auto* value = map.find({key.data ? key.data : "",static_cast<std::size_t>(key.length)});
        if (!value) return DFY_V2_NOT_FOUND;
        *output = store_object(host,std::shared_ptr<const runtime::Value>(parent,value));
        return DFY_V2_OK;
    });
}
// 读取版本 1 的标量映射字段约束。
dfy_v2_status DFY_V2_CALL field_type(dfy_v2_handle source, uint64_t index, dfy_v2_map_type_info* output, dfy_v2_error* error) {
    return field_info(source,index,output,error,1);
}
// 读取版本 2 的完整映射值分类。
dfy_v2_status DFY_V2_CALL field_type_v2(dfy_v2_handle source, uint64_t index, dfy_v2_map_type_info* output, dfy_v2_error* error) {
    return field_info(source,index,output,error,2);
}
// 保留版本 1 对组合值声明的拒绝。
dfy_v2_status DFY_V2_CALL describe(dfy_v2_handle source, dfy_v2_map_info* output, dfy_v2_error* error) {
    return describe_value(source,output,error,1);
}
// 返回版本 2 的实际映射信息。
dfy_v2_status DFY_V2_CALL describe_v2(dfy_v2_handle source, dfy_v2_map_info* output, dfy_v2_error* error) {
    return describe_value(source,output,error,2);
}
// 版本 1 只交付标量声明的条目，null 不绕过种类检查。
dfy_v2_status DFY_V2_CALL entry(dfy_v2_handle source, uint64_t index, dfy_v2_string_view* key, dfy_v2_handle* output, dfy_v2_error* error) {
    return entry_value(source,index,key,output,error,1);
}
// 版本 2 交付完整值种类及对应键借用。
dfy_v2_status DFY_V2_CALL entry_v2(dfy_v2_handle source, uint64_t index, dfy_v2_string_view* key, dfy_v2_handle* output, dfy_v2_error* error) {
    return entry_value(source,index,key,output,error,2);
}
// 版本 1 仅在标量映射中精确查询。
dfy_v2_status DFY_V2_CALL find(dfy_v2_handle source, dfy_v2_string_view key, dfy_v2_handle* output, dfy_v2_error* error) {
    return find_value(source,key,output,error,1);
}
// 版本 2 在完整映射中精确查询。
dfy_v2_status DFY_V2_CALL find_v2(dfy_v2_handle source, dfy_v2_string_view key, dfy_v2_handle* output, dfy_v2_error* error) {
    return find_value(source,key,output,error,2);
}
// 从实际映射取得匿名值约束的独立元数据持有。
// 步骤：清空输出 → 校验映射值 → 保持描述并别名引用匿名约束。
dfy_v2_status DFY_V2_CALL value_type(dfy_v2_handle source, dfy_v2_handle* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error,[&](auto& host) {
        const auto descriptor = object<runtime::Value>(host,source).as_map().descriptor();
        *output = store_object(host,std::shared_ptr<const runtime::FieldDescriptor>(descriptor,&descriptor->element().constraint()));
        return DFY_V2_OK;
    });
}
}
// 提供精确版本的字符串键映射服务。
const dfy_v2_map_api& map_api() {
    static const dfy_v2_map_api api = {{sizeof(dfy_v2_map_api),1},field_type,describe,entry,find};
    return api;
}
// 提供精确版本 2 的组合映射访问，不改变版本 1 的函数表。
const dfy_v2_map_api_v2& map_api_v2() {
    static const dfy_v2_map_api_v2 api = {{sizeof(dfy_v2_map_api_v2),2},field_type_v2,describe_v2,entry_v2,find_v2,value_type};
    return api;
}
}
