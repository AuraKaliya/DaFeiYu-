#include <dafeiyu/confirmation.hpp>
#include <dafeiyu/tool_schema.hpp>
#include <dafeiyu/run_journal.hpp>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace dfy::agent {
namespace {
constexpr std::size_t confirmation_bytes = 2*64*1024+2048;
// 用目标范围及规范参数识别同一动作，调用 ID、策略修订和插件重载不消除原拒绝。
bool same_action(const ConfirmationTarget& left, const ConfirmationTarget& right) {
    return left.goal_revision == right.goal_revision && left.tool_id == right.tool_id && left.arguments == right.arguments;
}
}
// 生成规范参数和完整版本绑定，不将确认当作工具授权来源。
// 步骤：核对调用名称和参数契约 → 规范化参数 → 复制宿主身份与期限。
ConfirmationTarget confirmation_target(const PolicyContext& context, const ToolSelection& selected, const model::ToolCall& call, runtime::DomainId candidate) {
    if (call.name != selected.description.model.name || call.id.empty() || call.id.size() > 128)
        throw std::invalid_argument("confirmation: invalid candidate");
    ToolSchema(selected.description.model.parameters).validate(call.arguments);
    return {context.attempt,context.goal_revision,context.policy_revision,selected.registration,selected.description.instance,
        selected.description.version,selected.description.id,call.id,call.name,nlohmann::json::parse(call.arguments).dump(),context.deadline,candidate,
        context.plan_revision,context.plan_step,context.observation_binding};
}
// 固定容量和宿主凭证，等待记录不取得任何工具资源。
ConfirmationBook::ConfirmationBook(RunId run, ConfirmationCaller caller, std::size_t limit, std::shared_ptr<runtime::Budget> parent, RunJournal* journal)
    : run_(run),caller_(caller),limit_(limit),journal_(journal) {
    if (!run.session || !run.sequence || !limit || limit > 64) throw std::invalid_argument("confirmation: invalid binding or capacity");
    entries_.reserve(limit);
    budget_ = std::make_shared<runtime::Budget>(runtime::BudgetAmount{limit,limit*confirmation_bytes},parent);
}
// 只为合法完整目标登记有限等待，拒绝动作不得靠新调用身份重新询问。
// 步骤：校验范围及容量 → 检查原目标拒绝记录 → 签发请求并保存值记录。
runtime::DomainId ConfirmationBook::offer(ConfirmationTarget target) {
    if (target.attempt.step.run != run_ || !target.attempt.step.sequence || !target.attempt.sequence || !target.goal_revision ||
        !target.policy_revision || !target.registration || !target.instance || !target.contract_version || target.tool_id.empty() ||
        target.tool_id.size() > 128 || target.call_id.empty() || target.call_id.size() > 128 || target.name.empty() ||
        target.name.size() > 64 || target.arguments.size() > 64*1024 || target.plan_revision > 16 || target.plan_step > 32 ||
        (static_cast<bool>(target.plan_revision) != static_cast<bool>(target.plan_step))) throw std::invalid_argument("confirmation: invalid target");
    const auto arguments = nlohmann::json::parse(target.arguments,nullptr,false);
    if (!arguments.is_object()) throw std::invalid_argument("confirmation: object arguments required");
    target.arguments = arguments.dump();
    if (target.arguments.size() > 64*1024) throw std::length_error("confirmation: canonical arguments too large");
    for (const auto& entry : entries_) {
        const auto& original = entry.value.target;
        if (entry.value.state == ConfirmationState::denied && same_action(original,target))
            throw std::logic_error("confirmation: action already denied for this goal");
    }
    if (entries_.size() == limit_) throw std::length_error("confirmation: request limit exhausted");
    auto charge = budget_->acquire({1,confirmation_bytes});
    const auto id = runtime::next_domain(); entries_.push_back({std::move(charge),{id,1,std::move(target)}}); publish(entries_.back()); return id;
}
// 只投影来源状态；账本生命周期必须位于宿主追踪记录生命周期内。
void ConfirmationBook::publish(const Entry& entry) noexcept {
    if (!journal_) return;
    const auto& value = entry.value; const auto& attempt = value.target.attempt; TraceFact fact;
    fact.subject = {attempt.step.run,TraceNodeKind::control,attempt.step.sequence,attempt.sequence,value.id};
    fact.kind = TraceFactKind::confirmation_changed; fact.confirmation = value.state; fact.source_revision = value.revision;
    fact.related = value.target.candidate; journal_->append(fact);
}
// 返回完整值副本，调用本身不会改变请求修订或触发执行。
std::vector<ConfirmationSnapshot> ConfirmationBook::snapshot() const {
    std::vector<ConfirmationSnapshot> result; result.reserve(entries_.size());
    for (const auto& entry : entries_) result.push_back(entry.value);
    return result;
}
// 只复制可公开的身份、修订和状态，不借用原确认记录或参数。
std::vector<ConfirmationSummary> ConfirmationBook::summaries() const {
    std::vector<ConfirmationSummary> result; result.reserve(entries_.size());
    for (const auto& entry : entries_) {
        const auto& value = entry.value; const auto& target = value.target;
        result.push_back({value.id,target.candidate,target.attempt,value.revision,target.goal_revision,target.policy_revision,value.state,
            target.plan_revision,target.plan_step,target.observation_binding});
    }
    return result;
}
// 读取等待数量，不以当前时钟推断或隐藏尚未推进的原记录状态。
std::size_t ConfirmationBook::waiting() const noexcept {
    std::size_t count = 0;
    for (const auto& entry : entries_) if (entry.value.state == ConfirmationState::waiting) ++count;
    return count;
}
// 按原请求定位记录，不接受跨运行的猜测身份。
ConfirmationBook::Entry& ConfirmationBook::find(runtime::DomainId request) {
    for (auto& entry : entries_) if (entry.value.id == request) return entry;
    throw std::invalid_argument("confirmation: unknown request");
}
// 只读取当前状态，旧的命令回执不是可执行令牌。
ConfirmationState ConfirmationBook::state(runtime::DomainId request) const {
    for (const auto& entry : entries_) if (entry.value.id == request) return entry.value.state;
    throw std::invalid_argument("confirmation: unknown request");
}
// 返回不可变原目标副本，不能由命令正文改变当前待审动作。
ConfirmationTarget ConfirmationBook::target(runtime::DomainId request) const {
    for (const auto& entry : entries_) if (entry.value.id == request) return entry.value.target;
    throw std::invalid_argument("confirmation: unknown request");
}
// 查询原决定回执，不重新授权或延长批准期限。
// 步骤：核对宿主凭证 → 查找原命令 → 完整比较命令后返回回执或冲突。
std::optional<ConfirmationReceipt> ConfirmationBook::prior_receipt(const ConfirmationCaller& caller, const ConfirmationCommand& command) const {
    if (!(caller == caller_) || !command.command) throw std::invalid_argument("confirmation: unbound caller or command");
    for (const auto& entry : entries_) if (entry.command && entry.command->command == command.command) {
        const auto& original = *entry.command;
        if (original.request != command.request || original.expected_revision != command.expected_revision ||
            original.expected != command.expected || original.approve != command.approve) throw std::invalid_argument("confirmation: command conflict");
        return entry.receipt;
    }
    return {};
}
// 在原值绑定上接纳一次宿主决定，重复命令不会改变当前状态。
// 步骤：复用或拒绝原命令 → 核对期限和修订 → 保存决定 → 撤销同目标同动作的未消费许可。
ConfirmationReceipt ConfirmationBook::decide(const ConfirmationCaller& caller, const ConfirmationCommand& command, ToolTime now) {
    if (const auto prior = prior_receipt(caller,command)) return *prior;
    advance(now); auto& entry = find(command.request); auto& value = entry.value;
    if (value.state != ConfirmationState::waiting || value.revision != command.expected_revision || value.target != command.expected)
        throw std::invalid_argument("confirmation: stale decision");
    entry.command = command;
    value.state = command.approve ? ConfirmationState::approved : ConfirmationState::denied; ++value.revision;
    if (!command.approve) for (auto& other : entries_) {
        if ((other.value.state == ConfirmationState::waiting || other.value.state == ConfirmationState::approved) &&
            same_action(other.value.target,value.target)) { other.value.state = ConfirmationState::denied; ++other.value.revision; publish(other); }
    }
    entry.receipt = {command.command,command.request,value.revision,value.state}; publish(entry); return entry.receipt;
}
// 仅消耗仍有效的单次批准，动作接纳后原批准不能用于再次执行。
// 步骤：更新到期状态 → 精确重查目标 → 消耗批准或返回拒绝。
bool ConfirmationBook::consume(runtime::DomainId request, const ConfirmationTarget& current, ToolTime now) {
    advance(now); auto& entry = find(request);
    if (entry.value.target != current) { invalidate(request,ConfirmationState::invalidated); return false; }
    if (entry.value.state != ConfirmationState::approved) return false;
    entry.value.state = ConfirmationState::consumed; ++entry.value.revision; publish(entry); return true;
}
// 失效只作用于未消费许可，不回滚已发生动作或用户拒绝。
void ConfirmationBook::invalidate(runtime::DomainId request, ConfirmationState reason) {
    if (reason != ConfirmationState::cancelled && reason != ConfirmationState::expired && reason != ConfirmationState::invalidated)
        throw std::invalid_argument("confirmation: invalid stop reason");
    auto& entry = find(request); auto& value = entry.value;
    if (value.state == ConfirmationState::waiting || value.state == ConfirmationState::approved) { value.state = reason; ++value.revision; publish(entry); }
}
// 逐项裁定停止边界，任何等待时长都不能变成同意。
void ConfirmationBook::advance(ToolTime now, runtime::CancelReason cancellation) {
    if (cancellation != runtime::CancelReason::none && cancellation != runtime::CancelReason::user &&
        cancellation != runtime::CancelReason::owner_stop && cancellation != runtime::CancelReason::deadline)
        throw std::invalid_argument("confirmation: invalid cancellation");
    for (auto& entry : entries_) {
        if (cancellation != runtime::CancelReason::none) invalidate(entry.value.id,
            cancellation == runtime::CancelReason::deadline ? ConfirmationState::expired : ConfirmationState::cancelled);
        else if (now >= entry.value.target.deadline) invalidate(entry.value.id,ConfirmationState::expired);
    }
}
}
