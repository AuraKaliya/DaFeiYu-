#pragma once
#include <string_view>

namespace dfy::runtime {
// 不分配内存地校验完整 UTF-8 标量序列，允许内嵌零，不规范化或截断。
bool valid_utf8(std::string_view text) noexcept;
}
