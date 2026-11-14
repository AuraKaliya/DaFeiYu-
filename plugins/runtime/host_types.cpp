#include "host_state.hpp"

namespace dfy::plugins::detail {
// 按数据能力版本显式映射已实现分类，避免绑定宿主枚举的内部数值。
uint32_t value_kind(runtime::ValueKind kind, uint32_t profile) {
    switch (kind) {
    case runtime::ValueKind::boolean: return DFY_V2_VALUE_BOOL;
    case runtime::ValueKind::int64: return DFY_V2_VALUE_INT64;
    case runtime::ValueKind::uint64: return DFY_V2_VALUE_UINT64;
    case runtime::ValueKind::float64: return DFY_V2_VALUE_FLOAT64;
    case runtime::ValueKind::string: return DFY_V2_VALUE_STRING;
    case runtime::ValueKind::bytes: return DFY_V2_VALUE_BYTES;
    case runtime::ValueKind::structure:
        if (profile == 2) return DFY_V2_VALUE_STRUCTURE;
        throw std::domain_error("host: kind unavailable in data capability v1");
    case runtime::ValueKind::null_value:
        if (profile == 2) return DFY_V2_VALUE_NULL;
        throw std::domain_error("host: null unavailable in data capability v1");
    case runtime::ValueKind::array:
        if (profile == 2) return DFY_V2_VALUE_ARRAY;
        throw std::domain_error("host: array unavailable in data capability v1");
    case runtime::ValueKind::map:
        if (profile == 2) return DFY_V2_VALUE_MAP;
        throw std::domain_error("host: map unavailable in data capability v1");
    case runtime::ValueKind::enumeration:
        if (profile == 2) return DFY_V2_VALUE_ENUM;
        throw std::domain_error("host: enum unavailable in data capability v1");
    case runtime::ValueKind::tagged_union:
        throw std::domain_error("host: union capability unavailable");
    }
    throw std::invalid_argument("host: unsupported value kind");
}
namespace {
// 返回结构描述元数据，字符串借用仍由原类型引用覆盖。
// 步骤：清空输出 → 校验绑定及引用种类 → 复制元数据并借用名称。
dfy_v2_status DFY_V2_CALL describe(dfy_v2_handle type, dfy_v2_type_info* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        const auto& descriptor = object<runtime::StructDescriptor>(host, type);
        *output = {{descriptor.name().data(), descriptor.name().size()}, descriptor.version(), 0, descriptor.fields().size()};
        return DFY_V2_OK;
    });
}
// 按精确数据版本读取声明字段及约束，越界或不支持时不返回部分信息。
// 步骤：清空输出 → 校验引用及索引 → 显式映射种类并返回借用。
dfy_v2_status field_info(dfy_v2_handle type, uint64_t index, dfy_v2_field_info* output, dfy_v2_error* error,
    uint32_t profile, uint32_t* flags = nullptr) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        const auto fields = object<runtime::StructDescriptor>(host, type).fields();
        if (index >= fields.size()) return DFY_V2_INVALID_ARGUMENT;
        const auto& entry = fields[static_cast<std::size_t>(index)];
        if (profile == 1 && (entry.optional || entry.nullable)) return DFY_V2_UNSUPPORTED;
        *output = {{entry.name.data(), entry.name.size()}, value_kind(entry.kind, profile), 0, entry.max_bytes};
        if (flags) *flags = (entry.optional ? DFY_V2_FIELD_OPTIONAL : 0) | (entry.nullable ? DFY_V2_FIELD_NULLABLE : 0);
        return DFY_V2_OK;
    });
}
// 保留版本 1 仅支持基础字段的语义。
dfy_v2_status DFY_V2_CALL field(dfy_v2_handle type, uint64_t index, dfy_v2_field_info* output, dfy_v2_error* error) {
    return field_info(type, index, output, error, 1);
}
// 通过版本 2 返回扩展分类及原有借用。
dfy_v2_status DFY_V2_CALL field_v2(dfy_v2_handle type, uint64_t index, dfy_v2_field_info_v2* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    dfy_v2_field_info info{};
    uint32_t flags = 0;
    const auto status = field_info(type, index, &info, error, 2, &flags);
    if (!status) *output = {info.name, info.kind, flags, info.max_bytes};
    return status;
}
// 为嵌套结构描述签发独立持有，不借用父描述引用。
// 步骤：清空输出 → 校验类型和字段 → 保留实际子描述。
dfy_v2_status DFY_V2_CALL nested(dfy_v2_handle type, uint64_t index, dfy_v2_handle* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        const auto fields = object<runtime::StructDescriptor>(host, type).fields();
        if (index >= fields.size() || !fields[static_cast<std::size_t>(index)].structure) return DFY_V2_INVALID_ARGUMENT;
        *output = store_object(host, fields[static_cast<std::size_t>(index)].structure);
        return DFY_V2_OK;
    });
}
}
// 提供固定布局的结构描述服务，不登记新类型或扩展字段种类。
const dfy_v2_type_api& type_api() {
    static const dfy_v2_type_api api = {{sizeof(dfy_v2_type_api), DFY_V2_TYPE_VERSION}, describe, field};
    return api;
}
// 提供精确版本 2 的类型反射，不更改版本 1 的表或行为。
const dfy_v2_type_api_v2& type_api_v2() {
    static const dfy_v2_type_api_v2 api = {{sizeof(dfy_v2_type_api_v2), 2}, describe, field_v2, nested};
    return api;
}
}
