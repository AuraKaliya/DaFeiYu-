#pragma once
#include <string>
#include <string_view>

namespace dfy::platform {
// 使用系统 SHA-256 计算指定字节的十六进制摘要，不读取文件、配置或凭据。
std::string sha256(std::string_view bytes);
}
