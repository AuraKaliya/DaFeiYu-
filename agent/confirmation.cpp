#include <dafeiyu/confirmation.hpp>
#include <dafeiyu/tool_schema.hpp>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace dfy::agent {
// 生成规范参数和完整版本绑定，不将确认当作工具授权来源。
// 步骤：核对调用名称和参数契约 → 规范化参数 → 复制宿主身份与期限。
ConfirmationTarget confirmation_target(const PolicyContext& context, const ToolSelection& selected, const model::ToolCall& call) {
    if (call.name != selected.description.model.name || call.id.empty() || call.id.size() > 128)
        throw std::invalid_argument("confirmation: invalid candidate");
    ToolSchema(selected.description.model.parameters).validate(call.arguments);
    return {context.attempt,context.goal_revision,context.policy_revision,selected.registration,selected.description.instance,
        selected.description.version,selected.description.id,call.id,call.name,nlohmann::json::parse(call.arguments).dump(),context.deadline};
}
// 固定容量和宿主凭证，等待记录不取得任何工具资源。
ConfirmationBook::ConfirmationBook(RunId run, ConfirmationCaller caller, std::size_t limit) : run_(run),caller_(caller),limit_(limit) {
    if (!run.session || !run.sequence || !limit || limit > 64) throw std::invalid_argument("confirmation: invalid binding or capacity");
    entries_.reserve(limit);
}
// 只为合法完整目标登记有限等待，拒绝动作不得靠新调用身份重新询问。
// 步骤：校验范围及容量 → 检查原目标拒绝记录 → 签发请求并保存值记录。
runtime::DomainId ConfirmationBook::offer(ConfirmationTarget target) {
    if (target.attempt.step.run != run_ || !target.attempt.step.sequence || !target.attempt.sequence || !target.goal_revision ||
        !target.policy_revision || !target.registration || !target.instance || !target.contract_version || target.tool_id.empty() ||
        target.tool_id.size() > 128 || target.call_id.empty() || target.call_id.size() > 128 || target.name.empty() ||
        target.name.size() > 64 || target.arguments.size() > 64*1024) throw std::invalid_argument("confirmation: invalid target");
    const auto arguments = nlohmann::json::parse(target.arguments,nullptr,false);
    if (!arguments.is_object()) throw std::invalid_argument("confirmation: object arguments required");
    target.arguments = arguments.dump();
    if (target.arguments.size() > 64*1024) throw std::length_error("confirmation: canonical arguments too large");
    for (const auto& entry : entries_) {
        const auto& original = entry.value.target;
        if (entry.value.state == ConfirmationState::denied && original.goal_revision == target.goal_revision &&
            original.tool_id == target.tool_id && original.arguments == target.arguments)
            throw std::logic_error("confirmation: action already denied for this goal");
    }
    if (entries_.size() == limit_) throw std::length_error("confirmation: request limit exhausted");
    const auto id = runtime::next_domain(); entries_.push_back({{id,1,std::move(target)}}); return id;
}
// 返回完整值副本，调用本身不会改变请求修订或触发执行。
std::vector<ConfirmationSnapshot> ConfirmationBook::snapshot() const {
    std::vector<ConfirmationSnapshot> result; result.reserve(entries_.size());
    for (const auto& entry : entries_) result.push_back(entry.value);
    return result;
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
// 在原值绑定上接纳一次宿主决定，重复命令不会改变当前状态。
// 步骤：校验宿主凭证及命令 → 复用或拒绝冲突回执 → 核对期限和修订 → 保存命令后发布决定。
ConfirmationReceipt ConfirmationBook::decide(const ConfirmationCaller& caller, const ConfirmationCommand& command, ToolTime now) {
    if (!(caller == caller_) || !command.command) throw std::invalid_argument("confirmation: unbound caller or command");
    for (const auto& entry : entries_) if (entry.command && entry.command->command == command.command) {
        const auto& original = *entry.command;
        if (original.request != command.request || original.expected_revision != command.expected_revision ||
            original.expected != command.expected || original.approve != command.approve) throw std::invalid_argument("confirmation: command conflict");
        return entry.receipt;
    }
    advance(now); auto& entry = find(command.request); auto& value = entry.value;
    if (value.state != ConfirmationState::waiting || value.revision != command.expected_revision || value.target != command.expected)
        throw std::invalid_argument("confirmation: stale decision");
    entry.command = command;
    value.state = command.approve ? ConfirmationState::approved : ConfirmationState::denied; ++value.revision;
    entry.receipt = {command.command,command.request,value.revision,value.state}; return entry.receipt;
}
// 仅消耗仍有效的单次批准，动作接纳后原批准不能用于再次执行。
// 步骤：更新到期状态 → 精确重查目标 → 消耗批准或返回拒绝。
bool ConfirmationBook::consume(runtime::DomainId request, const ConfirmationTarget& current, ToolTime now) {
    advance(now); auto& entry = find(request);
    if (entry.value.target != current) { invalidate(request,ConfirmationState::invalidated); return false; }
    if (entry.value.state != ConfirmationState::approved) return false;
    entry.value.state = ConfirmationState::consumed; ++entry.value.revision; return true;
}
// 失效只作用于未消费许可，不回滚已发生动作或用户拒绝。
void ConfirmationBook::invalidate(runtime::DomainId request, ConfirmationState reason) {
    if (reason != ConfirmationState::cancelled && reason != ConfirmationState::expired && reason != ConfirmationState::invalidated)
        throw std::invalid_argument("confirmation: invalid stop reason");
    auto& value = find(request).value;
    if (value.state == ConfirmationState::waiting || value.state == ConfirmationState::approved) { value.state = reason; ++value.revision; }
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
