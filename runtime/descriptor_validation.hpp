#pragma once
#include <dafeiyu/struct_descriptor.hpp>

namespace dfy::runtime::detail {
struct DescriptorMetrics { std::size_t depth, total_fields, name_bytes; };
// 核定命名描述及全部子约束的展开计量，不创建或登记类型快照。
DescriptorMetrics validate_description(std::string_view name, std::uint32_t version,
    std::span<const FieldDescriptor> fields, DescriptorLimits limits);
// 核定容器匿名元素约束，禁止名称和缺失，计量包含容器层及一个元素描述条目。
DescriptorMetrics validate_element(const FieldDescriptor& field, DescriptorLimits limits);
// 比较单个内容约束的名称、标志、精确种类及完整子定义。
bool equivalent_field(const FieldDescriptor& a, const FieldDescriptor& b) noexcept;
}
