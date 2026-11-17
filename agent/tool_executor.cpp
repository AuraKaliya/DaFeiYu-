#include <dafeiyu/tool_executor.hpp>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace dfy::agent {
namespace {
constexpr std::size_t record_bytes = 3*64*1024+256;
// 规范化完整参数以比较重复命令，保持 JSON 数值类型，不进行字符串转数值。
std::string canonical(const std::string& input) {
    if (input.size() > 64*1024) throw std::length_error("tools: arguments exceed 64 KiB");
    const auto value = nlohmann::json::parse(input,nullptr,false);
    if (!value.is_object()) throw std::invalid_argument("tools: object arguments required");
    return value.dump();
}
}
// 建立运行级调用上限、分层预算与独立顺序域，不执行模型或工具。
// 步骤：校验运行及限额 → 预留回执槽 → 建立预算和顺序域。
ToolExecutor::ToolExecutor(RunId run, ToolSnapshot tools, ToolLimits limits, std::shared_ptr<runtime::Budget> parent, ToolClock now, std::function<bool()> action_guard)
    : run_(run), tools_(std::move(tools)), limits_(limits), now_(std::move(now)),action_guard_(std::move(action_guard)) {
    if (!run.session || !run.sequence || !limits.calls || limits.calls > 64 || !now_ ||
        limits.timeout <= std::chrono::milliseconds::zero() || limits.timeout > std::chrono::hours(1))
        throw std::invalid_argument("tools: invalid execution limits");
    entries_.resize(limits.calls);
    budget_ = std::make_shared<runtime::Budget>(runtime::BudgetAmount{limits.calls,limits.calls*record_bytes},parent);
    domain_ = std::make_shared<runtime::SerialDomain>(limits.calls);
}
// 关闭运行的工具入口，驱动析构仍独立保留未退出工作。
ToolExecutor::~ToolExecutor() { close(); }
// 接纳前完成权限、幂等回执、重复效果和预算检查，不在提交期间执行动作。
// 步骤：复用或拒绝同 ID 命令 → 校验范围与重复效果 → 预留预算及顺序 → 发布驱动与回执。
std::shared_ptr<ToolRequest> ToolExecutor::submit(AttemptId attempt, model::ToolCall call) {
    using namespace runtime;
    if (advancing_) throw std::logic_error("tools: reentrant admission");
    if (attempt.step.run != run_ || !attempt.step.sequence || !attempt.sequence || call.id.empty() || call.id.size() > 128)
        throw std::invalid_argument("tools: invalid command identity");
    auto arguments = canonical(call.arguments);
    for (const auto& entry : entries_) if (entry.driver) {
        const auto request = entry.driver->request(); const auto& original = request->invocation();
        if (original.attempt != attempt || original.call.id != call.id) continue;
        if (original.call.name != call.name || entry.canonical != arguments)
            throw std::invalid_argument("tools: command identity conflict");
        return request;
    }
    if (!accepting_) throw std::logic_error("tools: admission closed");
    const auto selection = tools_.resolve(call.name,call.arguments);
    if (selection.description.changes_state && !selection.description.idempotent) {
        for (const auto& entry : entries_) if (entry.driver && entry.registration == selection.registration && entry.canonical == arguments) {
            const auto request = entry.driver->request();
            if (request->exit()->state() == OperationState::pending || request->outcome().effect == ToolEffect::unknown ||
                (request->operation()->state() != OperationState::succeeded && request->outcome().effect == ToolEffect::applied))
                throw std::logic_error("tools: prior matching action requires effect confirmation");
        }
    }
    if (size_ == entries_.size()) throw std::length_error("tools: run call limit exhausted");
    const auto now = now_(); const auto duration = std::chrono::duration_cast<ToolTime::duration>(limits_.timeout);
    if (now > ToolTime::max()-duration) throw std::overflow_error("tools: deadline overflow");
    auto charge = budget_->acquire({1,record_bytes});
    const auto ordering = domain_->submit();
    try {
        ToolResources resources{domain_,ordering,std::move(charge),now+duration,now_};
        if (selection.description.changes_state) resources.before_start = action_guard_;
        auto driver = tools_.prepare(attempt,std::move(call),std::move(resources));
        const auto request = driver->request();
        entries_[size_] = {std::move(driver),std::move(arguments),selection.registration};
        ++size_; return request;
    } catch (...) {
        if (ordering->phase() != SerialPhase::cancelled && ordering->withdrawal_reason() == CancelReason::none)
            domain_->withdraw(ordering,CancelReason::owner_stop);
        throw;
    }
}
// 推进既有顺序域与有限工作，回调不能重入同一执行循环。
// 步骤：保护推进入口 → 授予顺序资格 → 推进各受理工作 → 解除入口保护。
void ToolExecutor::advance() {
    if (advancing_) throw std::logic_error("tools: reentrant advance");
    advancing_ = true;
    try {
        domain_->advance();
        for (auto& entry : entries_) if (entry.driver) entry.driver->advance();
    } catch (...) { advancing_ = false; throw; }
    advancing_ = false;
}
// 关闭只请求取消，不自动重放命令或强行确认工作退出。
void ToolExecutor::close(runtime::CancelReason reason) {
    using namespace runtime;
    if (reason != CancelReason::user && reason != CancelReason::owner_stop && reason != CancelReason::deadline)
        throw std::invalid_argument("tools: cancellation reason required");
    accepting_ = false;
    for (const auto& entry : entries_) if (entry.driver) entry.driver->request()->request_cancel(reason);
    domain_->close(SerialClose::cancel);
}
// 查询实际退出状态，不推动任何工作或释放保留回执。
bool ToolExecutor::busy() const noexcept {
    for (const auto& entry : entries_) if (entry.driver && entry.driver->request()->exit()->state() == runtime::OperationState::pending) return true;
    return false;
}
}
