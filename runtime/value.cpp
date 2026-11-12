#include <dafeiyu/value.hpp>

namespace dfy::runtime {
// 接收宿主已构造的不可变结构，不允许空指针伪装成结构或 null。
Value Value::from_structure(std::shared_ptr<const StructValue> value) {
    if (!value) throw std::invalid_argument("value: nonempty structure required");
    return Value(std::move(value));
}
namespace {
// 校验 UTF-8 标量编码，拒绝过长编码、代理项、截断和超过 Unicode 上界的值。
// 步骤：识别首字节长度 → 核对剩余字节 → 校验连续字节及边界 → 推进到下一字符。
bool valid_utf8(std::string_view text) {
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

// 在上限内保存独立 UTF-8 文本，允许内嵌零且不执行规范化或截断。
// 步骤：核对字节上限 → 校验完整编码 → 复制到宿主不可变存储。
Value Value::from_string(std::string_view text, std::size_t max_bytes) {
    if (text.size() > max_bytes) throw std::length_error("value: string byte limit exceeded");
    if (!valid_utf8(text)) throw std::invalid_argument("value: valid UTF-8 required");
    return Value(text.empty() ? std::string{} : std::string(text));
}

// 在上限内保存独立二进制数据，不借用输入缓冲。
// 步骤：核对字节上限 → 按空或非空输入构造宿主存储。
Value Value::from_bytes(std::span<const std::uint8_t> bytes, std::size_t max_bytes) {
    if (bytes.size() > max_bytes) throw std::length_error("value: binary byte limit exceeded");
    return Value(bytes.empty() ? Bytes{} : Bytes(bytes.begin(), bytes.end()));
}
}
