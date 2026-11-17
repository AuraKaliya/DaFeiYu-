#pragma once
#include <dafeiyu/plan_book.hpp>
#include <dafeiyu/observation_verification.hpp>

namespace dfy::agent {
struct RunLoopState;
// 原步骤实际退出后由宿主签发的反馈请求；复制只保留原条件，不能改写目标或创建执行许可。
class PlanFeedbackTicket {
  public:
    // 复制原反馈请求身份和不可变条件，不重新采样或签发请求。
    PlanFeedbackTicket(const PlanFeedbackTicket&) = default;
    // 转交完整反馈值，返回请求时不再分配绑定数据。
    PlanFeedbackTicket(PlanFeedbackTicket&&) noexcept = default;
    // 禁止原位替换已经签发的反馈身份。
    PlanFeedbackTicket& operator=(const PlanFeedbackTicket&) = delete;
    // 返回原计划步骤，不从当前同名步骤猜测目标。
    const PlanStepKey& step() const noexcept { return step_; }
    // 返回仅用于证据关联的进程内请求身份，不能代替调用者凭证。
    runtime::DomainId id() const noexcept { return id_; }
    // 核对请求之后的新采样，不执行动作或自行完成计划。
    ObservationAssessment assess(const ObservationGuard& observation, ObservationTime now) const {
        if (!condition_) throw std::invalid_argument("feedback: moved request has no condition");
        return condition_->assess(observation,now,true,requested_);
    }
  private:
    friend struct RunLoopState;
    // 完整准备原步骤和判定条件后签发身份，只有已退出工作边界能够调用。
    PlanFeedbackTicket(PlanStepKey step, ObservationGuard baseline, std::string field, runtime::Value expected, ObservationTime requested)
        : step_(std::move(step)),condition_(std::make_shared<const ObservationExpectation>(std::move(baseline),std::move(field),expected)),
          requested_(requested),id_(runtime::next_domain()) {}
    PlanStepKey step_;
    std::shared_ptr<const ObservationExpectation> condition_;
    ObservationTime requested_;
    runtime::DomainId id_;
};
}
