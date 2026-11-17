#pragma once
#include <dafeiyu/observation_inbox.hpp>

namespace dfy::agent {
enum class ObservationMatch { satisfied, not_satisfied, unknown };
enum class ObservationEvidenceIssue { none, stale, wrong_object, untrusted, not_resampled, schema_changed, missing_field, type_changed };
struct ObservationAssessment {
    ObservationMatch match = ObservationMatch::unknown;
    ObservationEvidenceIssue issue = ObservationEvidenceIssue::none;
    runtime::DomainId source = 0;
    std::uint64_t revision = 0;
};
// 宿主定义的顶层标量等值条件，不解释模型完成声明或通用可执行表达式。
class ObservationExpectation {
  public:
    // 固定原对象、schema、字段和期望；首版只接受 Bool、Int64、UInt64、String 和显式 Null。
    ObservationExpectation(ObservationGuard baseline, std::string field, runtime::Value expected);
    // 复制同一不可变条件及计费基线，复制不授予新的来源权限。
    ObservationExpectation(const ObservationExpectation&) = default;
    // 禁止原位替换已经绑定的判定条件。
    ObservationExpectation& operator=(const ObservationExpectation&) = delete;
    // 按原对象检查当前状态；需要反馈时额外要求更高采样修订及不早于指定时刻。
    ObservationAssessment assess(const ObservationGuard& current, ObservationTime now, bool newer = false,
        ObservationTime sampled_after = ObservationTime::min()) const;
  private:
    ObservationGuard baseline_;
    std::string field_;
    runtime::Value expected_;
};
}
