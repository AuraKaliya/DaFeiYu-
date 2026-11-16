#pragma once
#include <dafeiyu/observation_inbox.hpp>
#include <dafeiyu/run_identity.hpp>
#include <functional>

namespace dfy::agent {
struct ObservationRule {
    runtime::DomainId source = 0;
    ObservationObject object;
    std::uint64_t revision = 1, goal_revision = 1;
    std::size_t maximum_runs = 1;
    std::chrono::milliseconds minimum_interval{1000};
    ObservationTime deadline;
};
enum class TriggerDecision { ready, disabled, stopped, expired, busy, run_limit, wrong_object, stale, gap, duplicate, throttled, invalid_clock, missing_observation, user_pending };
struct TriggerTicket {
    runtime::DomainId rule = 0, ticket = 0;
    std::uint64_t observation_revision = 0;
    // 完整比较预留身份，不能只按修订号复用另一个规则的接纳责任。
    bool operator==(const TriggerTicket&) const = default;
};
struct TriggerOffer { TriggerDecision decision; std::optional<TriggerTicket> ticket; };
struct TriggerReceipt { TriggerTicket ticket; RunId run; };
// 同宿主线程的有限触发规则；默认关闭，只预留和记录接纳，不自行调用模型或工具。
class ObservationTrigger {
  public:
    // 固定来源、对象、修订和有限窗口，并预留最多 64 次运行回执。
    ObservationTrigger(ObservationRule rule, ObservationTime created);
    // 停止未来触发，使已交付的控制门也观察到停止。
    ~ObservationTrigger();
    // 禁止复制触发次数、水位和停止责任。
    ObservationTrigger(const ObservationTrigger&) = delete;
    // 禁止赋值或移动活动规则。
    ObservationTrigger& operator=(const ObservationTrigger&) = delete;
    // 显式开启原规则，已停止的规则不能恢复或重置预算。
    void enable();
    // 永久关闭本规则；未完成预留仍须提交实际接纳或明确撤回。
    void stop() noexcept;
    // 读取固定规则，正文和模型返回值不能更新权限或预算。
    const ObservationRule& rule() const noexcept { return rule_; }
    // 返回已实际接纳运行的次数，失败、取消和成功都计入上限。
    std::size_t runs() const noexcept { return receipts_.size(); }
    // 返回与具体采样无关的规则接纳状态，不推进或消耗预算。
    TriggerDecision availability(ObservationTime now) const noexcept;
    // 只判断当前采样能否触发，不占用预算或修改水位。
    TriggerDecision inspect(const ObservationRecord& value, ObservationTime now) const noexcept;
    // 在调用 Session 前预留唯一接纳位置，拒绝不消耗观察修订或运行次数。
    TriggerOffer reserve(const ObservationRecord& value, ObservationTime now);
    // 记录已经接纳的原运行，停止或到期不能让已发生的接纳逃出计数；同票据复用原回执。
    TriggerReceipt commit(TriggerTicket ticket, RunId run, ObservationTime admitted);
    // 只有尚未实际接纳的预留可以撤回，之后允许同一采样重新尝试。
    void abort(TriggerTicket ticket);
    // 返回仅核对开启、停止及截止的值门；规则销毁后门仍安全地拒绝后续步骤。
    std::function<bool(ObservationTime)> guard() const;
  private:
    struct Control { bool enabled = false, stopped = false; };
    struct Pending { TriggerTicket ticket; ObservationTime reserved; };
    const ObservationRule rule_;
    const ObservationTime created_;
    runtime::DomainId id_;
    std::shared_ptr<Control> control_ = std::make_shared<Control>();
    std::optional<Pending> pending_;
    std::optional<ObservationTime> next_allowed_;
    std::vector<TriggerReceipt> receipts_;
};
}
