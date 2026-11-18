#pragma once
#include <dafeiyu/session.hpp>

namespace dfy::app {
// 对一个获准的读、改、读序列采集宿主证据，并使用原 Session 验证计划步骤。
class ToolVerification {
  public:
    // 固定两个工具、预期值及原提供实例，观察仅从真实只读回执生成。
    ToolVerification(std::string reader, std::string change, std::string expected, runtime::DomainId instance, agent::ObservationTime deadline);
    // 只有原读取实际退出且观察仍有效时，才允许批准指定动作。
    bool can_approve(const agent::ConfirmationTarget& target, agent::ObservationTime now) const noexcept;
    // 从原运行推进证据收集，不发起模型或工具调用。
    void advance(agent::Session& session, const agent::TurnRequest& run, const agent::ConfirmationCaller& caller);
    // 标记无法核验，保留已经完成的明确判定。
    void unavailable() noexcept { if (!match_) match_ = agent::ObservationMatch::unknown; }
    // 返回现有判定，缺少值表示仍在等待证据。
    std::optional<agent::ObservationMatch> match() const noexcept { return match_; }
  private:
    // 从已退出的真实只读回执发布同一对象上的新状态。
    agent::ObservationGuard observe(const agent::TurnRequest& run, const agent::BatchToolSnapshot& tool, std::uint64_t revision,
        agent::ObservationTime now);

    const std::string reader_, change_;
    const runtime::Value expected_;
    const runtime::DomainId instance_;
    const agent::ObservationTime deadline_;
    agent::ObservationSource source_;
    agent::ObservationInbox inbox_;
    std::optional<agent::ObservationGuard> baseline_;
    std::optional<agent::AttemptId> initial_attempt_;
    std::optional<agent::PlanFeedbackTicket> feedback_;
    std::optional<agent::ObservationMatch> match_;
};
}
