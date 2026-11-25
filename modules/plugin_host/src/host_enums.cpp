#include "host_state.hpp"
#include <dafeiyu/enum_value.hpp>

namespace dfy::plugins::detail {
namespace {
// 按精确符号复制枚举数字，不经过浮点转换，其他成员保持全零。
dfy_v2_value_view number_view(const runtime::EnumNumber& number) {
    dfy_v2_value_view result{};
    if (const auto* signed_number = std::get_if<std::int64_t>(&number)) {
        result.kind = DFY_V2_VALUE_INT64; result.data.int64 = *signed_number;
    } else {
        result.kind = DFY_V2_VALUE_UINT64; result.data.uint64 = std::get<std::uint64_t>(number);
    }
    return result;
}
// 为声明中的枚举字段签发独立描述引用，失败不遗留输出。
// 步骤：清空输出 → 核对结构字段 → 保持枚举描述。
dfy_v2_status DFY_V2_CALL field_type(dfy_v2_handle type, uint64_t index, dfy_v2_handle* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }

    return boundary(error, [&](auto& host) {
        const auto fields = object<runtime::StructDescriptor>(host, type).fields();
        if (index >= fields.size() || !fields[static_cast<std::size_t>(index)].enumeration) return DFY_V2_INVALID_ARGUMENT;
        *output = store_object(host, fields[static_cast<std::size_t>(index)].enumeration);
        return DFY_V2_OK;
    });
}
// 读取闭合枚举身份与整数种类，名称视图依赖传入的描述引用。
// 步骤：清空输出 → 校验描述种类 → 复制元数据并借用名称。
dfy_v2_status DFY_V2_CALL describe(dfy_v2_handle type, dfy_v2_enum_info* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }

    return boundary(error, [&](auto& host) {
        const auto& descriptor = object<runtime::EnumDescriptor>(host, type);
        *output = {{descriptor.name().data(),descriptor.name().size()},descriptor.version(),value_kind(descriptor.underlying()),descriptor.members().size()};
        return DFY_V2_OK;
    });
}
// 按声明顺序返回成员名借用与完整整数副本。
// 步骤：清空输出 → 校验描述及索引 → 复制成员视图。
dfy_v2_status DFY_V2_CALL member(dfy_v2_handle type, uint64_t index, dfy_v2_enum_member* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }

    return boundary(error, [&](auto& host) {
        const auto members = object<runtime::EnumDescriptor>(host, type).members();
        if (index >= members.size()) return DFY_V2_INVALID_ARGUMENT;
        const auto& selected = members[static_cast<std::size_t>(index)];
        *output = {{selected.name.data(),selected.name.size()},number_view(selected.number)};
        return DFY_V2_OK;
    });
}
// 为实际枚举值的描述签发独立持有，不把数据预算转交给描述。
// 步骤：清空输出 → 校验枚举值 → 保持独立描述。
dfy_v2_status DFY_V2_CALL descriptor(dfy_v2_handle value, dfy_v2_handle* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }

    return boundary(error, [&](auto& host) {
        *output = store_object(host, object<runtime::Value>(host,value).as_enum().descriptor());
        return DFY_V2_OK;
    });
}
// 读取不可变成员选择，不自动签发或释放数据引用。
// 步骤：清空输出 → 校验枚举值 → 复制索引及精确数字。
dfy_v2_status DFY_V2_CALL read(dfy_v2_handle value, dfy_v2_enum_selection* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }

    return boundary(error, [&](auto& host) {
        const auto& selected = object<runtime::Value>(host,value).as_enum();
        *output = {selected.index(),number_view(selected.member().number)};
        return DFY_V2_OK;
    });
}
}
// 返回精确版本的闭合枚举反射及读取服务。
const dfy_v2_enum_api& enum_api() {
    static const dfy_v2_enum_api api = {{sizeof(dfy_v2_enum_api),1},field_type,describe,member,descriptor,read};
    return api;
}
}
