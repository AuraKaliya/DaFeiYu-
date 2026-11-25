#pragma once
#include <cstdint>
#include <limits>

namespace dfy::events::detail {
// 推进成功接纳后的序号，最大值之后永久停止签发，零状态不重新启用。
inline constexpr std::uint64_t following_sequence(std::uint64_t current) noexcept {
    return !current || current == std::numeric_limits<std::uint64_t>::max() ? 0 : current + 1;
}
}
