#include <dafeiyu/union_value.hpp>
#include "field_validation.hpp"
#include <algorithm>

namespace dfy::runtime {
// 建立与选中分支完整约束一致的联合值，失败不返回部分选择。
// 步骤：核对描述及标签 → 验证内容约束 → 计算标签与实际内容预算 → 保存不可变值。
UnionValue UnionValue::create(std::shared_ptr<const UnionDescriptor> descriptor, std::uint64_t tag, const Value& value,
    std::size_t max_payload_bytes, std::size_t max_nodes) {
    if (!descriptor) throw std::invalid_argument("union: descriptor required");
    const auto index = descriptor->index_of(tag);
    if (!index) throw std::invalid_argument("union: declared tag required");
    detail::validate_field(descriptor->branches()[*index],value);

    const auto nodes = std::min(max_nodes,Value::maximum_nodes);
    if (!nodes || value.node_count() > nodes - 1) throw std::length_error("union: node limit exceeded");
    if (max_payload_bytes < 8 || value.payload_bytes() > max_payload_bytes - 8)
        throw std::length_error("union: payload byte limit exceeded");
    return UnionValue(std::move(descriptor),*index,value,8 + value.payload_bytes(),1 + value.node_count());
}
}
