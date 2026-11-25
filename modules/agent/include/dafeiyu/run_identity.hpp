#pragma once
#include <dafeiyu/runtime_ids.hpp>

namespace dfy::agent {
// 进程期运行身份；会话域与该会话接纳序号共同唯一，零值表示尚未接纳。
struct RunId {
    runtime::DomainId session = 0;
    std::uint64_t sequence = 0;
    // 比较会话和接纳序号，不根据业务状态或对象地址判断身份。
    bool operator==(const RunId&) const = default;
};
// 运行内的步骤身份；序号从一开始，不跨运行单独比较。
struct StepId {
    RunId run;
    std::uint64_t sequence = 0;
    // 同时比较所属运行和步骤序号。
    bool operator==(const StepId&) const = default;
};
// 步骤内的一次尝试身份；不代表尝试成功，也不拥有执行或退出状态。
struct AttemptId {
    StepId step;
    std::uint64_t sequence = 0;
    // 同时比较所属步骤和尝试序号。
    bool operator==(const AttemptId&) const = default;
};
}
