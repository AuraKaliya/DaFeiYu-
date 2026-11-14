#include "host_state.hpp"
#include <dafeiyu/union_value.hpp>

namespace dfy::plugins::detail {
namespace {
// 读取联合身份及分支数，不改变描述持有责任。
// 步骤：清空输出 → 校验联合描述 → 复制元数据并借用名称。
dfy_v2_status DFY_V2_CALL describe(dfy_v2_handle source, dfy_v2_union_info* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        const auto& type = object<runtime::UnionDescriptor>(host,source);
        *output = {{type.name().data(),type.name().size()},type.version(),0,type.branches().size()};
        return DFY_V2_OK;
    });
}
// 签发分支约束独立持有后才提交标签，分配失败不留下部分输出。
// 步骤：清空双输出 → 核对描述和索引 → 保持分支约束 → 复制稳定标签。
dfy_v2_status DFY_V2_CALL branch(dfy_v2_handle source, uint64_t index, uint64_t* tag, dfy_v2_handle* output, dfy_v2_error* error) {
    if (tag) *tag = 0;
    if (output) *output = {};
    if (!tag || !output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        const auto& parent = std::get<std::shared_ptr<const runtime::UnionDescriptor>>(host.references.get(native(source)).object);
        if (index >= parent->branches().size()) return DFY_V2_INVALID_ARGUMENT;
        const auto position = static_cast<std::size_t>(index);
        *output = store_object(host,std::shared_ptr<const runtime::FieldDescriptor>(parent,&parent->branches()[position]));
        *tag = parent->tag(position);
        return DFY_V2_OK;
    });
}
// 为联合值的描述签发独立引用，不把正文预算交给元数据。
// 步骤：清空输出 → 校验联合值 → 保持实际描述。
dfy_v2_status DFY_V2_CALL descriptor(dfy_v2_handle source, dfy_v2_handle* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        *output = store_object(host,object<runtime::Value>(host,source).as_union().descriptor());
        return DFY_V2_OK;
    });
}
// 复制联合的实际选择，不访问未选中分支的数据。
// 步骤：清空输出 → 校验联合值 → 复制索引与 UInt64 标签。
dfy_v2_status DFY_V2_CALL read(dfy_v2_handle source, dfy_v2_union_selection* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        const auto& value = object<runtime::Value>(host,source).as_union();
        *output = {value.index(),value.tag()};
        return DFY_V2_OK;
    });
}
// 以宿主别名保持联合内容，叶引用持续承担原始载荷和读取结果预算。
// 步骤：清空输出 → 校验联合值 → 签发保持父链的内容引用。
dfy_v2_status DFY_V2_CALL content(dfy_v2_handle source, dfy_v2_handle* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        const auto& parent = std::get<std::shared_ptr<const runtime::Value>>(host.references.get(native(source)).object);
        *output = store_object(host,std::shared_ptr<const runtime::Value>(parent,&parent->as_union().value()));
        return DFY_V2_OK;
    });
}
}
// 返回已实现的联合描述、分支、选择及内容访问能力。
const dfy_v2_union_api& union_api() {
    static const dfy_v2_union_api api = {{sizeof(dfy_v2_union_api),1},describe,branch,descriptor,read,content};
    return api;
}
}
