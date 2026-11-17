#pragma once
#include <dafeiyu/observation.hpp>
#include <dafeiyu/budget.hpp>

namespace dfy::agent {
enum class ObservationAdmission { inserted, replaced, duplicate, conflict, stale_revision, stale_generation, not_allowed, future, expired, source_revoked, capacity, stale_refresh };
struct ObservationReceipt { ObservationAdmission status; bool revision_gap = false; };
// 宿主开始完整重采样时固定的原水位；不是模型或载荷可以授予的行动许可。
struct ObservationRefresh {
    runtime::DomainId inbox = 0, source = 0;
    ObservationObject object;
    std::uint64_t revision = 0;
};
class ObservationInbox;
class ObservationGuard;
struct ObservationWatermark;
// 计费的数据持有；只借用载荷，客户端保留本句柄时旧版本继续占用预算。
class ObservationRecord {
  public:
    // 禁止复制计费责任；客户端共享同一记录句柄。
    ObservationRecord(const ObservationRecord&) = delete;
    // 禁止赋值替换已经发布的采样值。
    ObservationRecord& operator=(const ObservationRecord&) = delete;
    // 借用不可变来源与对象身份，借用期不得超过本记录持有期。
    const ObservationMetadata& metadata() const noexcept { return value_.metadata(); }
    // 借用不可变结构载荷，读取不会推进或重新采样。
    const runtime::StructValue& payload() const noexcept { return *value_.payload(); }
    // 核对原来源和时间资格，不把旧值保留解释为仍然新鲜。
    ObservationValidity validity(ObservationTime now) const noexcept { return value_.validity(now); }
    // 返回尚未用完整重采样处理的修订断档；普通连续采样不会静默清除此标记。
    bool revision_gap() const noexcept { return gap_; }
  private:
    friend class ObservationInbox;
    friend class ObservationGuard;
    // 接收已预留预算和值，数据持有先释放，最后归还本记录计费。
    ObservationRecord(runtime::BudgetLease charge, ObservationSnapshot value, bool gap, std::shared_ptr<ObservationWatermark> watermark)
        : charge_(std::move(charge)),value_(value),gap_(gap),watermark_(std::move(watermark)) {}
    runtime::BudgetLease charge_;
    ObservationSnapshot value_;
    bool gap_;
    std::shared_ptr<ObservationWatermark> watermark_;
};
// 固定观察值的当前性检查；只读水位和来源状态，不执行重采样或延长集合寿命。
class ObservationGuard {
  public:
    // 复制同一观察及集合寿命检查，复制不产生新权限。
    ObservationGuard(const ObservationGuard&) = default;
    // 替换本地检查值，不修改原集合或采样。
    ObservationGuard& operator=(const ObservationGuard&) = default;
    // 返回原有计费观察持有，旧数据可读不代表仍允许据此行动。
    std::shared_ptr<const ObservationRecord> observation() const noexcept { return observation_; }
    // 核对集合存活、对象水位、原来源及有效期；查询不产生任何外部动作。
    bool current(ObservationTime now) const noexcept;
  private:
    friend class ObservationInbox;
    // 只由原集合在当前且完整采样上签发检查，客户端不能自报水位。
    ObservationGuard(std::shared_ptr<const ObservationRecord> observation, std::weak_ptr<const bool> alive)
        : observation_(std::move(observation)),alive_(std::move(alive)) {}
    std::shared_ptr<const ObservationRecord> observation_;
    std::weak_ptr<const bool> alive_;
};
// 同宿主线程的最新状态采样集合；不接收用户命令或事务回执，不自动触发行动。
class ObservationInbox {
  public:
    // 固定来源白名单及对象槽；保留记录预算包括客户端持有的旧版本，至少预留一个替换槽。
    ObservationInbox(std::vector<runtime::DomainId> sources, std::size_t objects = 32,
        runtime::BudgetAmount retained = {64,256*1024}, std::shared_ptr<runtime::Budget> parent = {});
    // 集合销毁后使原检查失效，外部旧值仍按其持有计费。
    ~ObservationInbox();
    // 禁止复制接纳水位和所有者计费责任。
    ObservationInbox(const ObservationInbox&) = delete;
    // 禁止赋值或移动覆盖已发布采样。
    ObservationInbox& operator=(const ObservationInbox&) = delete;
    // 接纳新鲜且获准的状态，按来源／对象保留更高代次或修订；失败保持原值和水位。
    ObservationReceipt accept(const ObservationSnapshot& value, ObservationTime now);
    // 为已有对象记录完整重采样的期望水位，不触发来源调用。
    std::optional<ObservationRefresh> refresh_ticket(runtime::DomainId source, std::string_view object) const;
    // 宿主确认载荷为完整重采样后显式调用；原水位仍匹配且新修订接纳成功才清除断档。
    ObservationReceipt refresh(const ObservationRefresh& expected, const ObservationSnapshot& value, ObservationTime now);
    // 读取最新保留版本；过期值仍可回顾，调用方须同时检查时效及断档。
    std::shared_ptr<const ObservationRecord> latest(runtime::DomainId source, std::string_view object) const noexcept;
    // 为本集合当前、新鲜且无断档的原值签发检查；跨集合或旧版本拒绝。
    ObservationGuard guard(std::shared_ptr<const ObservationRecord> observation, ObservationTime now) const;
  private:
    // 共用来源、时效、版本及预算校验；完整基线仅由受保护的刷新入口选择。
    ObservationReceipt accept_impl(const ObservationSnapshot& value, ObservationTime now, bool baseline);
    runtime::DomainId id_;
    const std::vector<runtime::DomainId> sources_;
    std::shared_ptr<runtime::Budget> budget_;
    std::vector<std::shared_ptr<const ObservationRecord>> records_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
}
