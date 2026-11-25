#pragma once
#include <dafeiyu/struct_descriptor.hpp>

namespace dfy::runtime {
// 不可变匿名元素约束，复用全部已实现内容种类，不能省略元素但可允许 null。
class ElementDescriptor {
  public:
    // 校验匿名内容和展开预算；容器层算一层深度，元素约束本身算一个描述条目。
    static ElementDescriptor create(const FieldDescriptor& constraint, DescriptorLimits limits);
    // 复制匿名约束，子描述保持宿主不可变共享快照。
    ElementDescriptor(const ElementDescriptor&) = default;
    // 禁止修改已发布的内容约束。
    ElementDescriptor& operator=(const ElementDescriptor&) = delete;
    // 借用完整内容约束，名称为空且 optional 固定为 false。
    const FieldDescriptor& constraint() const noexcept { return constraint_; }
    // 返回包含容器层和嵌套内容的深度，标量或枚举元素对应一层。
    std::size_t container_depth() const noexcept { return depth_; }
    // 返回一个元素描述条目及全部子描述的展开数量，不乘以实际容器长度。
    std::size_t total_fields() const noexcept { return total_fields_; }
    // 返回所有命名子描述的展开名称正文，匿名元素自身不占名称字节。
    std::size_t name_bytes() const noexcept { return name_bytes_; }
    // 比较精确种类、可空、正文和全部子定义，不把验证预算当作契约字段。
    bool equivalent(const ElementDescriptor& other) const noexcept;
  private:
    // 保存已校验的匿名约束与完整展开计量。
    ElementDescriptor(FieldDescriptor constraint, std::size_t depth, std::size_t fields, std::size_t names)
        : constraint_(std::move(constraint)), depth_(depth), total_fields_(fields), name_bytes_(names) {}
    const FieldDescriptor constraint_;
    const std::size_t depth_, total_fields_, name_bytes_;
};
}
