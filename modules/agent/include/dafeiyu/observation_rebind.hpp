#pragma once
#include <dafeiyu/action_observations.hpp>
#include <dafeiyu/observation_input.hpp>
#include <dafeiyu/plan_book.hpp>

namespace dfy::agent {
struct RunLoopState;
// 原步骤实际退出后由宿主签发的刷新请求，保存请求水位而不授予动作权限。
class ObservationRebindTicket {
  public:
    // 复制同一个请求，不重新签发时刻或预算。
    ObservationRebindTicket(const ObservationRebindTicket&) = default;
    // 转交请求值；被移动的原值不再有可用基线。
    ObservationRebindTicket(ObservationRebindTicket&&) noexcept = default;
    // 禁止原位替换已签发身份。
    ObservationRebindTicket& operator=(const ObservationRebindTicket&) = delete;
    // 返回原步骤完整版本，不能作用于同名新步骤。
    const PlanStepKey& step() const noexcept { return step_; }
    // 返回请求身份，仅用于幂等关联。
    runtime::DomainId id() const noexcept { return id_; }
    // 返回待替换的原行动绑定，移动后为零。
    runtime::DomainId binding() const noexcept { return checkpoint_ ? checkpoint_->binding() : 0; }
    // 只判断新观察是否完整且晚于请求水位，不推进或恢复原运行。
    bool accepts(const ActionObservations& next, ObservationTime now) const { return checkpoint_ && checkpoint_->accepts(next,now); }
  private:
    friend struct RunLoopState;
    // 在原运行的退出边界准备全部基线后签发不可伪造的宿主请求。
    ObservationRebindTicket(PlanStepKey step, ActionObservations previous, ObservationTime now)
        : step_(std::move(step)),checkpoint_(std::make_shared<const ActionObservationCheckpoint>(std::move(previous),now)),id_(runtime::next_domain()) {}
    PlanStepKey step_;
    std::shared_ptr<const ActionObservationCheckpoint> checkpoint_;
    runtime::DomainId id_;
};
// 原替换接纳回执，只有无正文的关联信息，历史重取不能重新启用已消费依据。
struct ObservationRebindReceipt {
    runtime::DomainId request = 0, previous_binding = 0, binding = 0;
    PlanStepKey step;
    std::vector<ObservationInputFact> observations;
    std::size_t after_model_requests = 0;
};
}
