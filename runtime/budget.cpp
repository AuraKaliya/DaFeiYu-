#include <dafeiyu/budget.hpp>
#include <stdexcept>

namespace dfy::runtime {
// 固定父级和深度，限制元数据层数并拒绝无意义条目上限。
Budget::Budget(BudgetAmount limit, const std::shared_ptr<Budget>& parent)
    : control_(std::make_shared<BudgetLease::Control>(BudgetLease::Control{limit,{},parent ? parent->control_ : nullptr,
        parent ? parent->control_->depth + 1 : 1})) {
    if (!limit.items || control_->depth > 16) throw std::invalid_argument("budget: positive items and at most sixteen levels required");
}
// 先检查整条链再执行不会失败的加计费，避免部分接纳或整数回绕。
// 步骤：核对请求 → 用减法逐层检查剩余额度 → 逐层计费 → 转交凭证。
BudgetLease Budget::acquire(BudgetAmount amount) {
    if (!amount.items) throw std::invalid_argument("budget: positive item charge required");
    for (auto node = control_; node; node = node->parent)
        if (amount.items > node->limit.items - node->used.items || amount.bytes > node->limit.bytes - node->used.bytes)
            throw std::length_error("budget: capacity exhausted");
    for (auto node = control_; node; node = node->parent) { node->used.items += amount.items; node->used.bytes += amount.bytes; }
    return BudgetLease(control_,amount);
}
// 归还既有责任后移动接收，不增加或重复释放任何层的额度。
BudgetLease& BudgetLease::operator=(BudgetLease&& other) noexcept {
    if (this != &other) { reset(); control_ = std::move(other.control_); amount_ = std::exchange(other.amount_,{}); }
    return *this;
}
// 逐层同步归还，本份凭证保持祖先控制块寿命直至结束。
void BudgetLease::reset() noexcept {
    for (auto node = control_; node; node = node->parent) { node->used.items -= amount_.items; node->used.bytes -= amount_.bytes; }
    control_.reset(); amount_ = {};
}
}
