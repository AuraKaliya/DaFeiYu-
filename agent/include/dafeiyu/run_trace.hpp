#pragma once
#include <dafeiyu/session.hpp>
#include <dafeiyu/trace_identity.hpp>

namespace dfy::agent {
struct TraceKey {
    std::string name_space;
    RunId run;
    // 比较诊断命名空间及原运行身份，不将跨进程序号当成相同运行。
    bool operator==(const TraceKey&) const = default;
};
// 所有状态都是原操作的值投影；时间相对本次模型提交，缺失时间不伪造。
struct TraceNode {
    TraceNodeId id;
    std::optional<TraceNodeId> parent;
    runtime::OperationState result = runtime::OperationState::pending, exit = runtime::OperationState::pending;
    std::optional<std::int64_t> submitted_ns, accepted_ns, call_started_ns, call_finished_ns, settled_ns, observed_exit_ns;
    std::optional<model::CallOutcome> call_outcome;
    std::optional<ToolEffect> effect;
    ToolError error = ToolError::none, fault = ToolError::none;
};
struct TraceEvent {
    std::uint64_t revision = 0;
    TraceNodeId subject;
    RunFactKind kind = RunFactKind::accepted;
    std::int64_t observed_ns = 0;
    runtime::OperationState run_result = runtime::OperationState::pending;
    runtime::CancelReason cancellation = runtime::CancelReason::none;
    model::CallOutcome model_outcome = model::CallOutcome::pending;
    TurnFailure run_failure = TurnFailure::none;
};
struct RunTrace {
    static constexpr std::uint32_t schema_version = 1;
    TraceKey key;
    TraceVersions versions;
    std::vector<TraceNode> nodes;
    std::vector<TraceEvent> events;
    TurnNotifications notifications;
};
// 投影当前单步骤会话；不推进工作、不读取正文，拒绝将未来多步骤快照误作单次路径。
RunTrace single_run_trace(const RunSnapshot& snapshot, std::string name_space, TraceVersions versions = {});
// 复制工具身份、结果和效果，时间暂缺时保持未知，不持有原工作或 DLL。
TraceNode tool_trace_node(const ToolRequest& request);
}
