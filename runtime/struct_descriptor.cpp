#include <dafeiyu/struct_descriptor.hpp>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <dafeiyu/array_value.hpp>
#include <dafeiyu/map_value.hpp>
#include <dafeiyu/enum_descriptor.hpp>
#include "descriptor_name.hpp"

namespace dfy::runtime {
namespace {
// 从名称预算中扣除实际字节，先比较再相减以避免累计长度溢出。
void consume_name(std::string_view name, std::size_t& remaining) {
    if (name.size() > remaining) throw std::length_error("descriptor: name byte limit exceeded");
    remaining -= name.size();
}
using KnownStructures = std::unordered_map<std::string, const StructDescriptor*>;
using KnownEnums = std::unordered_map<std::string, const EnumDescriptor*>;
// 核对枚举身份，拒绝同键不同种类或不同成员定义。
// 步骤：排除根及结构身份冲突 → 登记枚举身份 → 比较重复定义。
void check_enum(const EnumDescriptor& descriptor, std::string_view root, std::uint32_t version,
    const KnownStructures& structures, KnownEnums& enums) {
    const auto key = std::string(descriptor.name()) + "#" + std::to_string(descriptor.version());
    if ((descriptor.name() == root && descriptor.version() == version) || structures.contains(key))
        throw std::invalid_argument("descriptor: conflicting enum identity");
    const auto [entry, inserted] = enums.emplace(key, &descriptor);
    if (!inserted && !entry->second->equivalent(descriptor)) throw std::invalid_argument("descriptor: conflicting enum definition");
}
// 检查组合快照内的精确类型身份，拒绝递归契约与同键不同定义。
// 步骤：拒绝根身份回返 → 核对已见定义 → 递归检查尚未见过的子描述。
void check_nested(const StructDescriptor& descriptor, std::string_view root, std::uint32_t version,
    KnownStructures& known, KnownEnums& enums) {
    if (descriptor.name() == root && descriptor.version() == version) throw std::invalid_argument("descriptor: recursive type identity");
    const auto key = std::string(descriptor.name()) + "#" + std::to_string(descriptor.version());
    if (enums.contains(key)) throw std::invalid_argument("descriptor: conflicting type kind");
    const auto [entry, inserted] = known.emplace(key, &descriptor);
    if (!inserted) {
        if (!entry->second->equivalent(descriptor)) throw std::invalid_argument("descriptor: conflicting nested identity");
        return;
    }
    for (const auto& field : descriptor.fields()) {
        if (field.structure) check_nested(*field.structure, root, version, known, enums);
        if (field.enumeration) check_enum(*field.enumeration, root, version, known, enums);
    }
}
}

// 生成有界不可变结构描述，嵌套按逻辑展开计费且不允许递归契约。
// 步骤：核对预算和身份 → 校验字段及嵌套计量 → 拒绝重复或冲突身份 → 保存快照。
StructDescriptor StructDescriptor::create(std::string_view name, std::uint32_t version,
    std::span<const FieldDescriptor> fields, DescriptorLimits limits) {
    if (!limits.max_depth || limits.max_depth > maximum_depth) throw std::invalid_argument("descriptor: invalid depth limit");
    const auto field_limit = std::min(limits.max_fields, maximum_fields);
    const auto name_limit = std::min(limits.max_name_bytes, maximum_name_bytes);
    if (fields.size() > field_limit) throw std::length_error("descriptor: field limit exceeded");
    auto remaining_fields = field_limit - fields.size();
    auto remaining = name_limit;
    std::size_t depth = 1;
    consume_name(name, remaining);
    if (!version || !detail::valid_descriptor_name(name, true)) throw std::invalid_argument("descriptor: invalid type identity");

    std::unordered_set<std::string_view> names;
    for (const auto& field : fields) {
        consume_name(field.name, remaining);
        if (!detail::valid_descriptor_name(field.name, false)) throw std::invalid_argument("descriptor: invalid field name");
        if (field.kind != ValueKind::structure && field.structure) throw std::invalid_argument("descriptor: unexpected nested schema");
        if (field.kind != ValueKind::array && field.array) throw std::invalid_argument("descriptor: unexpected array schema");
        if (field.kind != ValueKind::map && field.map) throw std::invalid_argument("descriptor: unexpected map schema");
        if (field.kind != ValueKind::enumeration && field.enumeration) throw std::invalid_argument("descriptor: unexpected enum schema");
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
        default:
            throw std::invalid_argument("descriptor: unsupported field kind");
        }
        if (!names.insert(field.name).second) throw std::invalid_argument("descriptor: duplicate field");
    }

    KnownStructures known;
    KnownEnums enums;
    for (const auto& field : fields) {
        if (field.structure) check_nested(*field.structure, name, version, known, enums);
        if (field.enumeration) check_enum(*field.enumeration, name, version, known, enums);
    }
    return StructDescriptor(std::string(name), version, {fields.begin(), fields.end()}, depth,
        field_limit - remaining_fields, name_limit - remaining);
}
// 在已限定深度和规模的快照之间比较定义，重复引用同一快照直接匹配。
// 步骤：比较身份和数量 → 顺序比较字段约束 → 比较嵌套定义。
bool StructDescriptor::equivalent(const StructDescriptor& other) const noexcept {
    if (this == &other) return true;
    if (name_ != other.name_ || version_ != other.version_ || fields_.size() != other.fields_.size()) return false;
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        const auto& a = fields_[i]; const auto& b = other.fields_[i];
        if (a.name != b.name || a.kind != b.kind || a.max_bytes != b.max_bytes || a.optional != b.optional || a.nullable != b.nullable ||
            static_cast<bool>(a.structure) != static_cast<bool>(b.structure) || static_cast<bool>(a.array) != static_cast<bool>(b.array) ||
            static_cast<bool>(a.map) != static_cast<bool>(b.map) || static_cast<bool>(a.enumeration) != static_cast<bool>(b.enumeration)) return false;
        if (a.structure && !a.structure->equivalent(*b.structure)) return false;
        if (a.array && !a.array->equivalent(*b.array)) return false;
        if (a.map && !a.map->equivalent(*b.map)) return false;
        if (a.enumeration && !a.enumeration->equivalent(*b.enumeration)) return false;
    }
    return true;
}
}
