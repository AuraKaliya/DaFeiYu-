#pragma once
#include <dafeiyu/enum_descriptor.hpp>

namespace dfy::runtime {
// 枚举值只保存一个已声明成员，持有宿主描述，不保留插件地址。
class EnumValue {
  public:
    // 按精确整数种类选择已声明成员；空描述和未知数字均拒绝。
    static EnumValue create(std::shared_ptr<const EnumDescriptor> descriptor, EnumNumber number);
    // 复制成员选择并共享不可变描述。
    EnumValue(const EnumValue&) = default;
    // 禁止修改已经发布的成员选择。
    EnumValue& operator=(const EnumValue&) = delete;
    // 返回描述的独立持有，描述不依赖数据引用的存活。
    std::shared_ptr<const EnumDescriptor> descriptor() const noexcept { return descriptor_; }
    // 返回声明内的成员索引，不能代替稳定整数值。
    std::size_t index() const noexcept { return index_; }
    // 借用被选中的完整成员，使用期间须保持枚举值或描述持有。
    const EnumMember& member() const noexcept { return descriptor_->members()[index_]; }
  private:
    // 保存已确认存在的成员选择。
    EnumValue(std::shared_ptr<const EnumDescriptor> descriptor, std::size_t index)
        : descriptor_(std::move(descriptor)), index_(index) {}
    const std::shared_ptr<const EnumDescriptor> descriptor_;
    const std::size_t index_;
};
}
