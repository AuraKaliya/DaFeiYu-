#include "descriptor_validation.hpp"
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <dafeiyu/array_value.hpp>
#include <dafeiyu/map_value.hpp>
#include <dafeiyu/enum_descriptor.hpp>
#include <dafeiyu/union_descriptor.hpp>
#include <type_traits>
#include "descriptor_name.hpp"

namespace dfy::runtime::detail {
namespace {
// 从名称预算中扣除实际字节，先比较再相减以避免累计长度溢出。
void consume_name(std::string_view name, std::size_t& remaining) {
    if (name.size() > remaining) throw std::length_error("descriptor: name byte limit exceeded");
    remaining -= name.size();
}
using KnownDefinitions = std::unordered_map<std::string,
    std::variant<const StructDescriptor*, const EnumDescriptor*, const UnionDescriptor*>>;
// 遍历字段或分支中的命名描述，所有种类共用同一身份空间。
void check_fields(std::span<const FieldDescriptor> fields, std::string_view root, std::uint32_t version, KnownDefinitions& known);
// 核对完整描述身份，重复定义必须同种类且完整等价。
// 步骤：拒绝根身份回返 → 比较或登记已见身份 → 遍历结构字段或联合分支。
template<class Descriptor>
void check_definition(const Descriptor& descriptor, std::string_view root, std::uint32_t version, KnownDefinitions& known) {
    if (descriptor.name() == root && descriptor.version() == version) throw std::invalid_argument("descriptor: recursive type identity");
    const auto key = std::string(descriptor.name()) + "#" + std::to_string(descriptor.version());
    const auto [entry, inserted] = known.emplace(key, &descriptor);
    if (!inserted) {
        const auto* existing = std::get_if<const Descriptor*>(&entry->second);
        if (!existing || !(*existing)->equivalent(descriptor)) throw std::invalid_argument("descriptor: conflicting nested identity");
        return;
    }
    if constexpr (std::is_same_v<Descriptor, StructDescriptor>) check_fields(descriptor.fields(),root,version,known);
    if constexpr (std::is_same_v<Descriptor, UnionDescriptor>) check_fields(descriptor.branches(),root,version,known);
}
// 检查当前层的命名子描述，递归由统一身份校验器负责。
void check_fields(std::span<const FieldDescriptor> fields, std::string_view root, std::uint32_t version, KnownDefinitions& known) {
    for (const auto& field : fields) {
        if (field.structure) check_definition(*field.structure,root,version,known);
        if (field.enumeration) check_definition(*field.enumeration,root,version,known);
        if (field.tagged_union) check_definition(*field.tagged_union,root,version,known);
    }
}
}

namespace {
// 核定命名或匿名描述的完整展开计量，嵌套按逻辑展开计费且不允许递归契约。
// 步骤：核对预算和身份 → 校验字段及嵌套计量 → 拒绝重复或冲突身份 → 返回完整计量。
DescriptorMetrics validate_schema(std::string_view name, std::uint32_t version,
    std::span<const FieldDescriptor> fields, DescriptorLimits limits, bool named) {
    if (!limits.max_depth || limits.max_depth > StructDescriptor::maximum_depth) throw std::invalid_argument("descriptor: invalid depth limit");
    const auto field_limit = std::min(limits.max_fields, StructDescriptor::maximum_fields);
    const auto name_limit = std::min(limits.max_name_bytes, StructDescriptor::maximum_name_bytes);
    if (fields.size() > field_limit) throw std::length_error("descriptor: field limit exceeded");
    auto remaining_fields = field_limit - fields.size();
    auto remaining = name_limit;
    std::size_t depth = 1;
    consume_name(name, remaining);
    if (named && (!version || !valid_descriptor_name(name, true))) throw std::invalid_argument("descriptor: invalid type identity");

    std::unordered_set<std::string_view> names;
    for (const auto& field : fields) {
        consume_name(field.name, remaining);
        if (named && !valid_descriptor_name(field.name, false)) throw std::invalid_argument("descriptor: invalid field name");
        if (field.kind != ValueKind::structure && field.structure) throw std::invalid_argument("descriptor: unexpected nested schema");
        if (field.kind != ValueKind::array && field.array) throw std::invalid_argument("descriptor: unexpected array schema");
        if (field.kind != ValueKind::map && field.map) throw std::invalid_argument("descriptor: unexpected map schema");
        if (field.kind != ValueKind::enumeration && field.enumeration) throw std::invalid_argument("descriptor: unexpected enum schema");
        if (field.kind != ValueKind::tagged_union && field.tagged_union) throw std::invalid_argument("descriptor: unexpected union schema");
        switch (field.kind) {
        case ValueKind::string:
        case ValueKind::bytes:
            break;
        case ValueKind::boolean:
        case ValueKind::int64:
        case ValueKind::uint64:
        case ValueKind::float64:
            if (field.max_bytes) throw std::invalid_argument("descriptor: byte limit on numeric field");
            break;
        case ValueKind::structure:
            if (!field.structure || field.max_bytes) throw std::invalid_argument("descriptor: structure schema required without byte override");
            if (field.structure->depth() >= limits.max_depth || field.structure->total_fields() > remaining_fields ||
                field.structure->name_bytes() > remaining) throw std::length_error("descriptor: nested limit exceeded");
            remaining_fields -= field.structure->total_fields();
            remaining -= field.structure->name_bytes();
            depth = std::max(depth, field.structure->depth() + 1);
            break;
        case ValueKind::array:
            if (!field.array || field.max_bytes) throw std::invalid_argument("descriptor: array schema required without byte override");
            if (limits.max_depth < 2) throw std::length_error("descriptor: array depth limit exceeded");
            depth = std::max(depth, std::size_t{2});
            break;
        case ValueKind::map:
            if (!field.map || field.max_bytes) throw std::invalid_argument("descriptor: map schema required without byte override");
            if (limits.max_depth < 2) throw std::length_error("descriptor: map depth limit exceeded");
            depth = std::max(depth, std::size_t{2});
            break;
        case ValueKind::enumeration:
            if (!field.enumeration || field.max_bytes) throw std::invalid_argument("descriptor: enum schema required without byte override");
            if (field.enumeration->members().size() > remaining_fields || field.enumeration->name_bytes() > remaining)
                throw std::length_error("descriptor: enum limit exceeded");
            remaining_fields -= field.enumeration->members().size();
            remaining -= field.enumeration->name_bytes();
            break;
        case ValueKind::tagged_union:
            if (!field.tagged_union || field.max_bytes) throw std::invalid_argument("descriptor: union schema required without byte override");
            if (field.tagged_union->depth() >= limits.max_depth || field.tagged_union->total_fields() > remaining_fields ||
                field.tagged_union->name_bytes() > remaining) throw std::length_error("descriptor: union limit exceeded");
            remaining_fields -= field.tagged_union->total_fields();
            remaining -= field.tagged_union->name_bytes();
            depth = std::max(depth,field.tagged_union->depth() + 1);
            break;
        default:
            throw std::invalid_argument("descriptor: unsupported field kind");
        }
        if (!names.insert(field.name).second) throw std::invalid_argument("descriptor: duplicate field");
    }

    KnownDefinitions known;
    check_fields(fields,name,version,known);
    return {depth, field_limit - remaining_fields, name_limit - remaining};
}
}
// 为结构和联合核定完整命名描述，保留身份与字段名称校验。
DescriptorMetrics validate_description(std::string_view name, std::uint32_t version,
    std::span<const FieldDescriptor> fields, DescriptorLimits limits) {
    return validate_schema(name,version,fields,limits,true);
}
// 核定容器的匿名元素约束，不占用任何用户类型名称。
// 步骤：禁止名称及缺失 → 以匿名容器层校验完整内容和预算。
DescriptorMetrics validate_element(const FieldDescriptor& field, DescriptorLimits limits) {
    if (!field.name.empty() || field.optional) throw std::invalid_argument("element: anonymous non-optional constraint required");
    return validate_schema({},0,{&field,1},limits,false);
}
// 比较内容约束的完整定义，不以名称或共享指针相同替代约束相同。
// 步骤：比较基本种类和标志 → 比较所有实际子描述。
bool equivalent_field(const FieldDescriptor& a, const FieldDescriptor& b) noexcept {
    if (a.name != b.name || a.kind != b.kind || a.max_bytes != b.max_bytes || a.optional != b.optional || a.nullable != b.nullable ||
        static_cast<bool>(a.structure) != static_cast<bool>(b.structure) || static_cast<bool>(a.array) != static_cast<bool>(b.array) ||
        static_cast<bool>(a.map) != static_cast<bool>(b.map) || static_cast<bool>(a.enumeration) != static_cast<bool>(b.enumeration) ||
        static_cast<bool>(a.tagged_union) != static_cast<bool>(b.tagged_union)) return false;
    if (a.structure && !a.structure->equivalent(*b.structure)) return false;
    if (a.array && !a.array->equivalent(*b.array)) return false;
    if (a.map && !a.map->equivalent(*b.map)) return false;
    if (a.enumeration && !a.enumeration->equivalent(*b.enumeration)) return false;
    if (a.tagged_union && !a.tagged_union->equivalent(*b.tagged_union)) return false;
    return true;
}
}
