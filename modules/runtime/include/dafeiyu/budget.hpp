#pragma once
#include <cstddef>
#include <memory>
#include <utility>

namespace dfy::runtime {
struct BudgetAmount { std::size_t items = 0, bytes = 0; };
class Budget;
// 一份分层计费凭证；移动不重复计费，最后归还时同步释放整条所有者链预算。
class BudgetLease {
  public:
    // 创建不占预算的空凭证。
    BudgetLease() = default;
    // 归还本份计费，不执行外部清理或业务取消。
    ~BudgetLease() { reset(); }
    // 禁止复制预算占用，新增持有必须显式申请。
    BudgetLease(const BudgetLease&) = delete;
    // 禁止复制赋值重复归还同一份计费。
    BudgetLease& operator=(const BudgetLease&) = delete;
    // 转交整条计费责任并清空来源。
    BudgetLease(BudgetLease&& other) noexcept : control_(std::move(other.control_)), amount_(std::exchange(other.amount_,{})) {}
    // 归还旧计费后转交责任，自移动无操作。
    BudgetLease& operator=(BudgetLease&& other) noexcept;
    // 同步归还每层预算，空凭证可重复归还。
    void reset() noexcept;
    // 判断是否仍承担计费责任。
    explicit operator bool() const noexcept { return static_cast<bool>(control_); }
  private:
    friend class Budget;
    struct Control { BudgetAmount limit, used; std::shared_ptr<Control> parent; std::size_t depth; };
    // 接收已完成逐层计费的责任，不再分配内存。
    BudgetLease(std::shared_ptr<Control> control, BudgetAmount amount) noexcept : control_(std::move(control)), amount_(amount) {}
    std::shared_ptr<Control> control_;
    BudgetAmount amount_;
};

// 同宿主线程的条目与字节分层预算，最多 16 层；父级固定，不能形成依赖环。
class Budget {
  public:
    // 建立正条目上限及可选父级，零字节上限允许纯元数据记录。
    explicit Budget(BudgetAmount limit, const std::shared_ptr<Budget>& parent = {});
    // 禁止复制预算身份，多个资源可共享同一预算对象。
    Budget(const Budget&) = delete;
    // 禁止赋值或移动已被凭证使用的预算对象。
    Budget& operator=(const Budget&) = delete;
    // 原子检查并占用本级至根的额度，任一层不足不改变任何计量。
    BudgetLease acquire(BudgetAmount amount);
    // 返回本级当前计量副本，包含所有子级尚未归还的占用。
    BudgetAmount used() const noexcept { return control_->used; }
    // 返回创建时固定的本级限制。
    BudgetAmount limit() const noexcept { return control_->limit; }
  private:
    const std::shared_ptr<BudgetLease::Control> control_;
};
}
