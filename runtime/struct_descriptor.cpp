#include <dafeiyu/struct_descriptor.hpp>
#include <dafeiyu/array_value.hpp>
#include <dafeiyu/map_value.hpp>
#include <dafeiyu/enum_descriptor.hpp>
#include <dafeiyu/union_descriptor.hpp>
#include "descriptor_validation.hpp"

namespace dfy::runtime {
// 校验完整命名描述后复制快照，未通过约束时不发布任何描述。
// 步骤：核定身份、字段与展开计量 → 保存独立名称和字段快照。
StructDescriptor StructDescriptor::create(std::string_view name, std::uint32_t version,
    std::span<const FieldDescriptor> fields, DescriptorLimits limits) {
    const auto metrics = detail::validate_description(name,version,fields,limits);
    return StructDescriptor(std::string(name),version,{fields.begin(),fields.end()},
        metrics.depth,metrics.total_fields,metrics.name_bytes);
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
            static_cast<bool>(a.map) != static_cast<bool>(b.map) || static_cast<bool>(a.enumeration) != static_cast<bool>(b.enumeration) ||
            static_cast<bool>(a.tagged_union) != static_cast<bool>(b.tagged_union)) return false;
        if (a.structure && !a.structure->equivalent(*b.structure)) return false;
        if (a.array && !a.array->equivalent(*b.array)) return false;
        if (a.map && !a.map->equivalent(*b.map)) return false;
        if (a.enumeration && !a.enumeration->equivalent(*b.enumeration)) return false;
        if (a.tagged_union && !a.tagged_union->equivalent(*b.tagged_union)) return false;
    }
    return true;
}
}
