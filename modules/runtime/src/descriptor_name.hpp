#pragma once
#include <string_view>

namespace dfy::runtime::detail {
// 校验小写 ASCII 标识；完整类型名至少两段，局部名称只允许一段。
// 步骤：按段检查首字母与后续字符 → 拒绝空段 → 核对限定名称要求。
inline bool valid_descriptor_name(std::string_view name, bool qualified) {
    bool first = true, dotted = false;
    for (const char character : name) {
        if (character == '.' && qualified && !first) {
            first = true;
            dotted = true;
            continue;
        }
        if (!(character >= 'a' && character <= 'z') &&
            (first || !((character >= '0' && character <= '9') || character == '_'))) return false;
        first = false;
    }
    return !first && (!qualified || dotted);
}
}
