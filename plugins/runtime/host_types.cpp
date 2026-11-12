#include "host_state.hpp"

namespace dfy::plugins::detail {
// 显式映射六种已实现类型，避免绑定宿主枚举的内部数值。
uint32_t value_kind(runtime::ValueKind kind) {
    switch (kind) {
    case runtime::ValueKind::boolean: return DFY_V2_VALUE_BOOL;
    case runtime::ValueKind::int64: return DFY_V2_VALUE_INT64;
    case runtime::ValueKind::uint64: return DFY_V2_VALUE_UINT64;
    case runtime::ValueKind::float64: return DFY_V2_VALUE_FLOAT64;
    case runtime::ValueKind::string: return DFY_V2_VALUE_STRING;
    case runtime::ValueKind::bytes: return DFY_V2_VALUE_BYTES;
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
// 读取声明顺序中的基础字段，越界不返回部分信息。
// 步骤：清空输出 → 校验引用及索引 → 显式映射种类并返回借用。
dfy_v2_status DFY_V2_CALL field(dfy_v2_handle type, uint64_t index, dfy_v2_field_info* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        const auto fields = object<runtime::StructDescriptor>(host, type).fields();
        if (index >= fields.size()) return DFY_V2_INVALID_ARGUMENT;
        const auto& entry = fields[static_cast<std::size_t>(index)];
        *output = {{entry.name.data(), entry.name.size()}, value_kind(entry.kind), 0, entry.max_bytes};
        return DFY_V2_OK;
    });
}
}
// 提供固定布局的结构描述服务，不登记新类型或扩展字段种类。
const dfy_v2_type_api& type_api() {
    static const dfy_v2_type_api api = {{sizeof(dfy_v2_type_api), DFY_V2_TYPE_VERSION}, describe, field};
    return api;
}
}
