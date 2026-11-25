#include <dafeiyu/enum_value.hpp>

namespace dfy::runtime {
// 建立与描述一致的闭合枚举值，不隐式接受未知数字或转换符号。
// 步骤：校验描述持有 → 查询精确成员 → 保存不可变选择。
EnumValue EnumValue::create(std::shared_ptr<const EnumDescriptor> descriptor, EnumNumber number) {
    if (!descriptor) throw std::invalid_argument("enum: descriptor required");
    const auto index = descriptor->index_of(number);
    if (!index) throw std::invalid_argument("enum: declared number required");

    return EnumValue(std::move(descriptor), *index);
}
}
