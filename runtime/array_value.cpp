#include <dafeiyu/array_value.hpp>
#include <algorithm>

namespace dfy::runtime {
// 建立仅支持已实现标量元素的数组约束，不接受未来类型占位。
// 步骤：校验数量上限 → 校验元素种类及字节约束 → 保存快照。
ArrayDescriptor ArrayDescriptor::create(ValueKind kind, std::size_t max_elements, std::size_t max_element_bytes, bool nullable) {
    if (max_elements > maximum_elements) throw std::length_error("array: element limit exceeds hard bound");
    switch (kind) {
    case ValueKind::string:
    case ValueKind::bytes: break;
    case ValueKind::boolean:
    case ValueKind::int64:
    case ValueKind::uint64:
    case ValueKind::float64:
        if (max_element_bytes) throw std::invalid_argument("array: byte bound on numeric element");
        break;
    default: throw std::invalid_argument("array: scalar element kind required");
    }
    return ArrayDescriptor(kind, max_elements, max_element_bytes, nullable);
}

// 在所有约束通过后复制数组，拒绝时不返回半成品或修改输入。
// 步骤：检查描述、数量及节点 → 校验元素类型和正文 → 核对聚合预算 → 保存副本。
ArrayValue ArrayValue::create(std::shared_ptr<const ArrayDescriptor> descriptor, std::span<const Value> elements,
    std::size_t max_payload_bytes, std::size_t max_nodes) {
    if (!descriptor) throw std::invalid_argument("array: descriptor required");
    if (elements.size() > descriptor->max_elements() || elements.size() >= std::min(max_nodes, Value::maximum_nodes)) {
        throw std::length_error("array: element or node budget exceeded");
    }

    auto remaining = max_payload_bytes;
    for (const auto& element : elements) {
        if (element.kind() == ValueKind::null_value) {
            if (!descriptor->nullable()) throw std::invalid_argument("array: null not allowed");
        } else if (element.kind() != descriptor->element_kind()) throw std::invalid_argument("array: element kind mismatch");
        const auto bytes = element.payload_bytes();
        if ((element.kind() == ValueKind::string || element.kind() == ValueKind::bytes) && bytes > descriptor->max_element_bytes()) {
            throw std::length_error("array: element byte limit exceeded");
        }
        if (bytes > remaining) throw std::length_error("array: payload limit exceeded");
        remaining -= bytes;
    }

    auto owned = elements.empty() ? std::vector<Value>{} : std::vector<Value>(elements.begin(), elements.end());
    return ArrayValue(std::move(descriptor), std::move(owned), max_payload_bytes - remaining);
}
}
