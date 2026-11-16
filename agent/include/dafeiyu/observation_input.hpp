#pragma once
#include <dafeiyu/observation_inbox.hpp>

namespace dfy::agent {
// 运行接纳时固定的观察关联；普通诊断不包含载荷或源时钟点。
struct ObservationInputFact {
    runtime::DomainId source = 0, provider_instance = 0;
    ObservationObject object;
    std::uint64_t revision = 0;
    ObservationTrust trust = ObservationTrust::external_data;
};
// 将新鲜且无断档观察编码为最多 64 KiB 的数据封套；支持标量、结构、数组与映射，其他类型明确拒绝。
std::string observation_input(const ObservationRecord& observation, ObservationTime now);
}
