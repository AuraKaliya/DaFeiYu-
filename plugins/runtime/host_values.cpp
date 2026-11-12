#include "host_state.hpp"

namespace dfy::plugins::detail {
namespace {
// 在精确种类下读取值，先完成本地快照以免失败留下部分输出。
// 步骤：清空输出 → 核对绑定及值引用 → 按种类复制数值或借用正文 → 提交快照。
dfy_v2_status DFY_V2_CALL read(dfy_v2_handle source, dfy_v2_value_view* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        const auto& value = object<runtime::Value>(host, source);
        dfy_v2_value_view result{};
        result.kind = value_kind(value.kind());
        switch (value.kind()) {
        case runtime::ValueKind::boolean: result.data.boolean = value.as_bool() ? 1 : 0; break;
        case runtime::ValueKind::int64: result.data.int64 = value.as_int64(); break;
        case runtime::ValueKind::uint64: result.data.uint64 = value.as_uint64(); break;
        case runtime::ValueKind::float64: result.data.float64 = value.as_float64(); break;
        case runtime::ValueKind::string: { const auto text = value.as_string(); result.data.string = {text.data(), text.size()}; break; }
        case runtime::ValueKind::bytes: { const auto bytes = value.as_bytes(); result.data.bytes = {bytes.data(), bytes.size()}; break; }
        }
        *output = result;
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
dfy_v2_status DFY_V2_CALL field(dfy_v2_handle source, uint64_t index, dfy_v2_handle* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        const auto& parent = std::get<std::shared_ptr<const runtime::StructValue>>(host.references.get(native(source)).object);
        if (index >= parent->size()) return DFY_V2_INVALID_ARGUMENT;
        *output = store_object(host, std::shared_ptr<const runtime::Value>(parent, &parent->at(static_cast<std::size_t>(index))));
        return DFY_V2_OK;
    });
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
}
