#include <dafeiyu/struct_descriptor.hpp>
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
// 步骤：比较身份和数量 → 顺序比较每个字段的完整内容约束。
bool StructDescriptor::equivalent(const StructDescriptor& other) const noexcept {
    if (this == &other) return true;
    if (name_ != other.name_ || version_ != other.version_ || fields_.size() != other.fields_.size()) return false;

    for (std::size_t i = 0; i < fields_.size(); ++i) {
        if (!detail::equivalent_field(fields_[i],other.fields_[i])) return false;
    }
    return true;
}
}
