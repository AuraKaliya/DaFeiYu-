#pragma once
#include <dafeiyu/observation_inbox.hpp>

namespace dfy::agent {
// 将新鲜且无断档观察编码为最多 64 KiB 的数据封套；支持标量、结构、数组与映射，其他类型明确拒绝。
std::string observation_input(const ObservationRecord& observation, ObservationTime now);
}
