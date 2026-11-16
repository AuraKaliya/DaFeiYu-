#pragma once
#include <dafeiyu/confirmation.hpp>
#include <dafeiyu/trace_identity.hpp>
#include <dafeiyu/async_model.hpp>

namespace dfy::agent {
enum class TraceFactKind {
    accepted, model_submitted, model_exited, tool_candidate, tool_changed, confirmation_changed,
    policy_decision, history_committed, cancel_requested, settled, exited
};
// 固定大小的宿主观察事实，无正文、原生地址或可复用授权凭证；不持有完成权。
struct TraceFact {
    std::uint64_t revision = 0;
    TraceNodeId subject;
    TraceFactKind kind = TraceFactKind::accepted;
    std::chrono::steady_clock::time_point observed;
    runtime::OperationState result = runtime::OperationState::pending, exit = runtime::OperationState::pending;
    runtime::CancelReason cancellation = runtime::CancelReason::none;
    PolicyReason stop_reason = PolicyReason::none;
    std::optional<model::CallOutcome> model_outcome;
    std::optional<ToolEffect> effect;
    ToolError error = ToolError::none, fault = ToolError::none;
    std::optional<ControlPoint> control_point;
    std::optional<PolicyDecision> decision;
    std::optional<ConfirmationState> confirmation;
    std::uint64_t source_revision = 0, related = 0;
    std::size_t history_messages = 0;
};
struct TraceDelta {
    RunId run;
    std::uint64_t requested_after = 0, oldest_revision = 1, latest_revision = 0;
    bool gap = false, more = false, projection_failed = false;
    std::vector<TraceFact> facts;
};
// 同宿主线程的有界事实投影；写入不分配、不抛出，也不结算任何业务操作。
class RunJournal {
  public:
    // 在模型和工具开始前预留一至 1024 个事实槽。
    explicit RunJournal(RunId run, std::size_t capacity = 256);
    // 禁止复制修订来源及环形保留窗口。
    RunJournal(const RunJournal&) = delete;
    // 禁止赋值或移动覆盖既有事实来源。
    RunJournal& operator=(const RunJournal&) = delete;
    // 复制值事实并签发本来源修订；溢出裁剪最旧项，非法投影只记录故障。
    void append(TraceFact fact) noexcept;
    // 读取游标之后的有限事实，保留窗口外明确标记缺口，未来游标拒绝。
    TraceDelta read(std::uint64_t after = 0, std::size_t limit = 256) const;
    // 返回最新来源修订，不推进运行或消费消息。
    std::uint64_t revision() const noexcept { return revision_; }
  private:
    const RunId run_;
    std::vector<TraceFact> slots_;
    std::uint64_t revision_ = 0;
    std::size_t begin_ = 0, size_ = 0;
    bool failed_ = false;
};
}
