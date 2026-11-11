#pragma once
#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace dfy::runtime::detail {
// 从宿主计数器签发非零标识，满值后永久拒绝，不回绕或复用。
// 步骤：读取已签发上界 → 拒绝耗尽 → 竞争提交新上界并在冲突时重读。
inline std::uint64_t allocate_id(std::atomic<std::uint64_t>& last) {
    auto observed = last.load(std::memory_order_relaxed);
    for (;;) {
        if (observed == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("runtime: identifier space exhausted");
        }
        if (last.compare_exchange_weak(observed, observed + 1, std::memory_order_relaxed)) {
            return observed + 1;
        }
    }
}
}
