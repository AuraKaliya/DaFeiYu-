#pragma once
#include <dafeiyu/value.hpp>

namespace dfy::runtime {
// 第一版数组约束仅接受六种基础元素，可独立允许 null，不接受组合元素占位。
class ArrayDescriptor {
  public:
    static constexpr std::size_t maximum_elements = 65536;
    // 校验元素种类、数量及单元素正文上限，数值类型的字节上限必须为零。
    static ArrayDescriptor create(ValueKind element_kind, std::size_t max_elements, std::size_t max_element_bytes = 0, bool nullable = false);
    // 复制只读约束快照。
    ArrayDescriptor(const ArrayDescriptor&) = default;
    // 禁止修改已发布约束。
    ArrayDescriptor& operator=(const ArrayDescriptor&) = delete;
    // 返回声明的非空元素种类。
    ValueKind element_kind() const noexcept { return element_kind_; }
    // 返回元素个数上限，零仅允许空数组。
    std::size_t max_elements() const noexcept { return max_elements_; }
    // 返回每个 String/Bytes 元素的正文上限。
    std::size_t max_element_bytes() const noexcept { return max_element_bytes_; }
    // 判断是否独立允许显式 null 元素。
    bool nullable() const noexcept { return nullable_; }
    // 比较元素种类和全部约束，不以实际元素数量代替声明相同。
    bool equivalent(const ArrayDescriptor& other) const noexcept {
        return element_kind_ == other.element_kind_ && max_elements_ == other.max_elements_ &&
            max_element_bytes_ == other.max_element_bytes_ && nullable_ == other.nullable_;
    }
  private:
    // 保存已经校验的不可变数组约束。
    ArrayDescriptor(ValueKind kind, std::size_t count, std::size_t bytes, bool nullable)
        : element_kind_(kind), max_elements_(count), max_element_bytes_(bytes), nullable_(nullable) {}
    const ValueKind element_kind_;
    const std::size_t max_elements_, max_element_bytes_;
    const bool nullable_;
};

// 宿主持有的有序不可变数组，所有元素在构造时复制并核定预算。
class ArrayValue {
  public:
    // 校验数量、元素类型、可空及两层正文和节点预算后保存独立副本。
    static ArrayValue create(std::shared_ptr<const ArrayDescriptor> descriptor, std::span<const Value> elements,
        std::size_t max_payload_bytes, std::size_t max_nodes = Value::maximum_nodes);
    // 复制数组元素并共享不可变约束。
    ArrayValue(const ArrayValue&) = default;
    // 禁止修改已发布数组。
    ArrayValue& operator=(const ArrayValue&) = delete;
    // 取得约束的独立共享持有。
    std::shared_ptr<const ArrayDescriptor> descriptor() const noexcept { return descriptor_; }
    // 返回实际元素数，不等同于声明上限。
    std::size_t size() const noexcept { return elements_.size(); }
    // 按顺序借用元素，越界抛出 out_of_range，借用跟随数组引用。
    const Value& at(std::size_t index) const { return elements_.at(index); }
    // 返回已核定的逻辑正文总量。
    std::size_t payload_bytes() const noexcept { return payload_bytes_; }
    // 返回包含根容器和实际元素的逻辑节点数。
    std::size_t node_count() const noexcept { return elements_.size() + 1; }
  private:
    // 接收已校验的约束和元素副本。
    ArrayValue(std::shared_ptr<const ArrayDescriptor> descriptor, std::vector<Value> elements, std::size_t bytes)
        : descriptor_(std::move(descriptor)), elements_(std::move(elements)), payload_bytes_(bytes) {}
    const std::shared_ptr<const ArrayDescriptor> descriptor_;
    const std::vector<Value> elements_;
    const std::size_t payload_bytes_;
};
}
