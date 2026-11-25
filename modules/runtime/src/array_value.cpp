#include <dafeiyu/array_value.hpp>
#include <algorithm>
#include "field_validation.hpp"

namespace dfy::runtime {
// 通过基础工厂创建标量匿名元素，不凭种类编号伪造组合内容约束。
// 步骤：校验数量及标量种类 → 校验匿名元素 → 保存完整数组约束。
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
    return create(ElementDescriptor::create({"",kind,max_element_bytes,{},false,nullable},{1,0}),max_elements);
}
// 以已校验的完整匿名元素建立有界数组约束。
ArrayDescriptor ArrayDescriptor::create(const ElementDescriptor& element, std::size_t max_elements) {
    if (max_elements > maximum_elements) throw std::length_error("array: element limit exceeds hard bound");
    return ArrayDescriptor(element,max_elements);
}

// 在所有约束通过后复制数组，拒绝时不返回半成品或修改输入。
// 步骤：检查描述和数量 → 校验完整元素约束 → 累计正文与逻辑节点 → 保存副本。
ArrayValue ArrayValue::create(std::shared_ptr<const ArrayDescriptor> descriptor, std::span<const Value> elements,
    std::size_t max_payload_bytes, std::size_t max_nodes) {
    if (!descriptor) throw std::invalid_argument("array: descriptor required");
    const auto node_limit = std::min(max_nodes,Value::maximum_nodes);
    if (elements.size() > descriptor->max_elements() || elements.size() >= node_limit) {
        throw std::length_error("array: element or node budget exceeded");
    }

    auto remaining = max_payload_bytes;
    auto remaining_nodes = node_limit - 1;
    for (const auto& element : elements) {
        detail::validate_field(descriptor->element().constraint(),element);
        if (element.node_count() > remaining_nodes) throw std::length_error("array: node budget exceeded");
        remaining_nodes -= element.node_count();
        const auto bytes = element.payload_bytes();
        if (bytes > remaining) throw std::length_error("array: payload limit exceeded");
        remaining -= bytes;
    }

    auto owned = elements.empty() ? std::vector<Value>{} : std::vector<Value>(elements.begin(), elements.end());
    return ArrayValue(std::move(descriptor), std::move(owned), max_payload_bytes - remaining, node_limit - remaining_nodes);
}
}
