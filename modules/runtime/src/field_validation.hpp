#pragma once
#include <dafeiyu/struct_descriptor.hpp>

namespace dfy::runtime::detail {
// 校验单个实际内容的种类、完整描述与正文上限；缺失和聚合预算由所属容器处理。
void validate_field(const FieldDescriptor& field, const Value& value);
}
