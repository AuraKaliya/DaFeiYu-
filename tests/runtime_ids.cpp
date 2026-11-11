#include <dafeiyu/runtime_ids.hpp>
#include "../runtime/id_sequence.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <thread>

namespace {
// 在签发规则不满足时报告失败。
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

// 验证宿主域独立、并发不重复和整数边界，不消耗生产计数器的全部空间。
// 步骤：验证独立调用 → 并发签发并核对唯一性 → 用同一分配算法验证满值及重复拒绝。
int main() {
    try {
        const auto first = dfy::runtime::next_domain();
        const auto second = dfy::runtime::next_domain();
        check(first != 0 && second > first, "domain reused or empty");

        std::array<std::uint64_t, 512> identifiers{};
        {
            std::array<std::jthread, 4> workers;
            for (size_t worker = 0; worker < workers.size(); ++worker) {
                workers[worker] = std::jthread([&, worker] {
                    for (size_t index = worker * 128; index < (worker + 1) * 128; ++index) {
                        identifiers[index] = dfy::runtime::next_domain();
                    }
                });
            }
        }
        std::sort(identifiers.begin(), identifiers.end());
        check(identifiers.front() > second && std::adjacent_find(identifiers.begin(), identifiers.end()) == identifiers.end(),
            "concurrent domain collision");
        check(dfy::runtime::next_domain() > identifiers.back(), "counter reset after callers exited");

        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        std::atomic<std::uint64_t> boundary{maximum - 1};
        check(dfy::runtime::detail::allocate_id(boundary) == maximum, "last identifier was lost");
        for (int attempt = 0; attempt < 2; ++attempt) {
            bool rejected = false;
            try { (void)dfy::runtime::detail::allocate_id(boundary); }
            catch (const std::overflow_error&) { rejected = true; }
            check(rejected && boundary.load() == maximum, "exhausted counter wrapped");
        }
        std::cout << "Runtime identifier checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
