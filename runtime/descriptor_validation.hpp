#pragma once
#include <dafeiyu/struct_descriptor.hpp>

namespace dfy::runtime::detail {
struct DescriptorMetrics { std::size_t depth, total_fields, name_bytes; };
// 核定命名描述及全部子约束的展开计量，不创建或登记类型快照。
DescriptorMetrics validate_description(std::string_view name, std::uint32_t version,
    std::span<const FieldDescriptor> fields, DescriptorLimits limits);
}
