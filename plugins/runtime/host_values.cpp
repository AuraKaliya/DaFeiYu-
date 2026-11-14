#include "host_state.hpp"

namespace dfy::plugins::detail {
namespace {
// 在精确种类下读取值，先完成本地快照以免失败留下部分输出。
// 步骤：清空输出 → 核对绑定及值引用 → 按种类复制数值或借用正文 → 提交快照。
dfy_v2_status read_value(dfy_v2_handle source, dfy_v2_value_view* output, dfy_v2_error* error, uint32_t profile) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        const auto& value = object<runtime::Value>(host, source);
        dfy_v2_value_view result{};
        result.kind = value_kind(value.kind(), profile);
        switch (value.kind()) {
        case runtime::ValueKind::boolean: result.data.boolean = value.as_bool() ? 1 : 0; break;
        case runtime::ValueKind::int64: result.data.int64 = value.as_int64(); break;
        case runtime::ValueKind::uint64: result.data.uint64 = value.as_uint64(); break;
        case runtime::ValueKind::float64: result.data.float64 = value.as_float64(); break;
        case runtime::ValueKind::string: { const auto text = value.as_string(); result.data.string = {text.data(), text.size()}; break; }
        case runtime::ValueKind::bytes: { const auto bytes = value.as_bytes(); result.data.bytes = {bytes.data(), bytes.size()}; break; }
        case runtime::ValueKind::structure: break;
        case runtime::ValueKind::null_value: break;
        case runtime::ValueKind::array: break;
        case runtime::ValueKind::map: break;
        case runtime::ValueKind::enumeration: break;
        }
        *output = result;
        return DFY_V2_OK;
    });
}
// 按基础类型版本读取，组合类型仍明确拒绝。
dfy_v2_status DFY_V2_CALL read(dfy_v2_handle source, dfy_v2_value_view* output, dfy_v2_error* error) {
    return read_value(source, output, error, 1);
}
// 按版本 2 读取分类，组合数据通过专用访问接口取得。
dfy_v2_status DFY_V2_CALL read_v2(dfy_v2_handle source, dfy_v2_value_view* output, dfy_v2_error* error) {
    return read_value(source, output, error, 2);
}
// 将结构内容作为原值的别名持有，持续承担父载荷或读取结果预算。
dfy_v2_status DFY_V2_CALL structure(dfy_v2_handle source, dfy_v2_handle* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        const auto& parent = std::get<std::shared_ptr<const runtime::Value>>(host.references.get(native(source)).object);
        *output = store_object(host, std::shared_ptr<const runtime::StructValue>(parent, &parent->as_structure()));
        return DFY_V2_OK;
    });
}
// 显式持有结构描述，原结构引用随后释放不会使这个描述失效。
dfy_v2_status DFY_V2_CALL descriptor(dfy_v2_handle source, dfy_v2_handle* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        *output = store_object(host, object<runtime::StructValue>(host, source).descriptor());
        return DFY_V2_OK;
    });
}
// 以宿主别名持有字段，保持完整父载荷责任且不复制或借用调用方容器。
// 步骤：清空输出 → 核对结构引用及索引 → 建立保持父载荷的字段引用。
dfy_v2_status field_value(dfy_v2_handle source, uint64_t index, dfy_v2_handle* output, dfy_v2_error* error, uint32_t profile) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        const auto& parent = std::get<std::shared_ptr<const runtime::StructValue>>(host.references.get(native(source)).object);
        if (index >= parent->size()) return DFY_V2_INVALID_ARGUMENT;
        const auto& declared = parent->descriptor()->fields()[static_cast<std::size_t>(index)];
        if (profile == 1 && (declared.optional || declared.nullable)) return DFY_V2_UNSUPPORTED;
        if (!parent->present(static_cast<std::size_t>(index))) return DFY_V2_NOT_FOUND;
        (void)value_kind(parent->at(static_cast<std::size_t>(index)).kind(), profile);
        *output = store_object(host, std::shared_ptr<const runtime::Value>(parent, &parent->at(static_cast<std::size_t>(index))));
        return DFY_V2_OK;
    });
}
// 版本 1 仅签发可由基础值能力读取的字段。
dfy_v2_status DFY_V2_CALL field(dfy_v2_handle source, uint64_t index, dfy_v2_handle* output, dfy_v2_error* error) {
    return field_value(source, index, output, error, 1);
}
// 版本 2 可签发组合字段，持有责任与基础字段一致。
dfy_v2_status DFY_V2_CALL field_v2(dfy_v2_handle source, uint64_t index, dfy_v2_handle* output, dfy_v2_error* error) {
    return field_value(source, index, output, error, 2);
}
}
// 返回精确版本的基础值只读表。
const dfy_v2_value_api& value_api() {
    static const dfy_v2_value_api api = {{sizeof(dfy_v2_value_api), 1}, read};
    return api;
}
// 返回精确版本的结构引用访问表。
const dfy_v2_struct_api& struct_api() {
    static const dfy_v2_struct_api api = {{sizeof(dfy_v2_struct_api), 1}, descriptor, field};
    return api;
}
// 提供实际结构访问的值能力版本 2。
const dfy_v2_value_api_v2& value_api_v2() {
    static const dfy_v2_value_api_v2 api = {{sizeof(dfy_v2_value_api_v2), 2}, read_v2, structure};
    return api;
}
// 提供版本 2 的结构字段访问，不更改原版本表。
const dfy_v2_struct_api_v2& struct_api_v2() {
    static const dfy_v2_struct_api_v2 api = {{sizeof(dfy_v2_struct_api_v2), 2}, descriptor, field_v2};
    return api;
}
}
