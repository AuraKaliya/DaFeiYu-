#pragma once
#include <dafeiyu/execution_policy.hpp>

namespace dfy::agent {
// 宿主在入口绑定的进程内调用凭证；消息正文不能通过自报 ID 构造同一凭证。
class ConfirmationCaller {
  public:
    // 签发独立调用凭证，宿主可复制给自己的控制入口，不交给模型或普通观察者。
    ConfirmationCaller() : id_(runtime::next_domain()) {}
    // 比较宿主绑定身份，不接受字符串或数值身份转换。
    bool operator==(const ConfirmationCaller&) const = default;
  private:
    runtime::DomainId id_;
};
enum class ConfirmationState { waiting, approved, denied, cancelled, expired, invalidated, consumed };
struct ConfirmationTarget {
    AttemptId attempt;
    std::uint64_t goal_revision = 0, policy_revision = 0;
    runtime::DomainId registration = 0, instance = 0;
    std::uint32_t contract_version = 0;
    std::string tool_id, call_id, name, arguments;
    ToolTime deadline;
    // 完整比较动作绑定，参数采用规范 JSON，禁止仅按名称或摘要碰撞复用授权。
    bool operator==(const ConfirmationTarget&) const = default;
};
// 仅用于可信控制入口的值快照，含待审阅参数；普通诊断不得自动导出本对象。
struct ConfirmationSnapshot {
    runtime::DomainId id = 0;
    std::uint64_t revision = 1;
    ConfirmationTarget target;
    ConfirmationState state = ConfirmationState::waiting;
};
struct ConfirmationCommand {
    runtime::DomainId command = 0, request = 0;
    std::uint64_t expected_revision = 0;
    ConfirmationTarget expected;
    bool approve = false;
};
struct ConfirmationReceipt {
    runtime::DomainId command = 0, request = 0;
    std::uint64_t revision = 0;
    ConfirmationState state = ConfirmationState::waiting;
    // 比较原命令回执，重试返回原值而非新执行许可。
    bool operator==(const ConfirmationReceipt&) const = default;
};
// 从已授权候选生成完整动作绑定，保留规范参数以供确认与执行前精确比较。
ConfirmationTarget confirmation_target(const PolicyContext& context, const ToolSelection& selection, const model::ToolCall& call);
// 运行内有界确认账本，只拥有值记录，不占用工具顺序域、DLL 或业务完成权。
class ConfirmationBook {
  public:
    // 固定运行、宿主凭证及一至 64 条容量，每条记录和原命令均计入分层预算。
    ConfirmationBook(RunId run, ConfirmationCaller caller, std::size_t limit = 16, std::shared_ptr<runtime::Budget> parent = {});
    // 禁止复制确认状态，避免同一批准拥有多个消费入口。
    ConfirmationBook(const ConfirmationBook&) = delete;
    // 禁止赋值或移动覆盖已签发的确认记录。
    ConfirmationBook& operator=(const ConfirmationBook&) = delete;
    // 为当前候选创建等待记录；同目标已拒绝的动作不能换调用 ID 再次弹出。
    runtime::DomainId offer(ConfirmationTarget target);
    // 复制可信控制入口所需的完整记录，不推进或批准动作。
    std::vector<ConfirmationSnapshot> snapshot() const;
    // 返回已登记记录数，已终止记录仍占运行内回执容量。
    std::size_t size() const noexcept { return entries_.size(); }
    // 返回等待用户决定的条数，不导出参数或改变到期状态。
    std::size_t waiting() const noexcept;
    // 读取原记录状态，不从过去的批准回执推导当前许可。
    ConfirmationState state(runtime::DomainId request) const;
    // 复制单项原目标，宿主决定入口用其重新核对当前授权，不相信命令正文替换目标。
    ConfirmationTarget target(runtime::DomainId request) const;
    // 只读取同一宿主原命令回执，身份或命令冲突明确拒绝。
    std::optional<ConfirmationReceipt> prior_receipt(const ConfirmationCaller& caller, const ConfirmationCommand& command) const;
    // 按宿主身份和修订作决定；拒绝覆盖同目标同动作未消费许可，原命令重试返回相同回执。
    ConfirmationReceipt decide(const ConfirmationCaller& caller, const ConfirmationCommand& command, ToolTime now);
    // 在接纳动作前消耗精确批准，失效或期限到达均不能复用。
    bool consume(runtime::DomainId request, const ConfirmationTarget& current, ToolTime now);
    // 只使尚未消费的等待或批准失效，已执行动作事实由原工具操作保留。
    void invalidate(runtime::DomainId request, ConfirmationState reason);
    // 在停止或时间推进时封闭未消费记录，不因超时自动批准。
    void advance(ToolTime now, runtime::CancelReason cancellation = runtime::CancelReason::none);
  private:
    struct Entry { runtime::BudgetLease charge; ConfirmationSnapshot value; std::optional<ConfirmationCommand> command; ConfirmationReceipt receipt; };
    // 按签发身份定位有界记录，未知请求明确拒绝。
    Entry& find(runtime::DomainId request);
    const RunId run_;
    const ConfirmationCaller caller_;
    const std::size_t limit_;
    std::shared_ptr<runtime::Budget> budget_;
    std::vector<Entry> entries_;
};
}
