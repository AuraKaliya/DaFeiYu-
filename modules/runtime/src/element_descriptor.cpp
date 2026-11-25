#include <dafeiyu/element_descriptor.hpp>
#include "descriptor_validation.hpp"

namespace dfy::runtime {
// 保存经过统一验证的匿名元素快照，不引入保留类型名称或忽略字段标志。
// 步骤：核定匿名约束与完整计量 → 复制独立约束快照。
ElementDescriptor ElementDescriptor::create(const FieldDescriptor& constraint, DescriptorLimits limits) {
    const auto metrics = detail::validate_element(constraint,limits);
    return ElementDescriptor(constraint,metrics.depth,metrics.total_fields,metrics.name_bytes);
}
// 使用与结构字段相同的完整定义比较规则。
bool ElementDescriptor::equivalent(const ElementDescriptor& other) const noexcept {
    return detail::equivalent_field(constraint_,other.constraint_);
}
}
