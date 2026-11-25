#include <dafeiyu/utf8.hpp>

namespace dfy::runtime {
// 校验 UTF-8 标量编码，拒绝过长编码、代理项、截断和超过 Unicode 上界的值。
// 步骤：识别首字节长度 → 核对剩余字节 → 校验连续字节及边界 → 推进到下一字符。
bool valid_utf8(std::string_view text) noexcept {
    for (std::size_t offset = 0; offset < text.size();) {
        const auto first = static_cast<unsigned char>(text[offset]);
        if (first <= 0x7f) { ++offset; continue; }
        const std::size_t width = first >= 0xc2 && first <= 0xdf ? 2 :
            first >= 0xe0 && first <= 0xef ? 3 : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
        if (!width || text.size() - offset < width) return false;

        for (std::size_t index = 1; index < width; ++index) {
            const auto next = static_cast<unsigned char>(text[offset + index]);
            if (next < 0x80 || next > 0xbf) return false;
        }
        const auto second = static_cast<unsigned char>(text[offset + 1]);
        if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0) ||
            (first == 0xf0 && second < 0x90) || (first == 0xf4 && second >= 0x90)) return false;
        offset += width;
    }
    return true;
}
}
