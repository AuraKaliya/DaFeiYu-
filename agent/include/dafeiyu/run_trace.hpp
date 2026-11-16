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
// 有操作的节点读取原状态；步骤／尝试只分组，没有伪造终态。时间相对运行起点，缺失保持未知。
struct TraceNode {
    TraceNodeId id;
    std::optional<TraceNodeId> parent;
    std::optional<runtime::OperationState> result, exit;
    std::optional<std::int64_t> submitted_ns, accepted_ns, call_started_ns, call_finished_ns, settled_ns, observed_exit_ns;
    std::optional<model::CallOutcome> call_outcome;
    std::optional<ToolEffect> effect;
    ToolError error = ToolError::none, fault = ToolError::none;
    std::optional<model::Usage> usage;
    std::optional<bool> accepted;
    runtime::DomainId related = 0;
    std::optional<PolicyDecision> before, after;
    std::optional<ConfirmationState> confirmation;
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
// 进程内完整当前节点与有限历史增量；origin 和 delta 时刻属于同一单调时钟域，不能跨进程比较。
struct RunTimeline {
    static constexpr std::uint32_t schema_version = 2;
    TraceKey key;
    TraceVersions versions;
    TurnDiagnostics::TimePoint origin;
    std::vector<TraceNode> nodes;
    TraceDelta delta;
    TurnNotifications notifications;
    TurnFailure failure = TurnFailure::none;
    PolicyReason stop_reason = PolicyReason::none;
    RunOrigin source = RunOrigin::user_request;
    std::uint64_t goal_revision = 0;
    std::vector<ObservationInputFact> observations;
};
// 投影正式多步当前事实并分页读取历史；历史缺口不会隐藏当前模型、工具及确认状态。
RunTimeline run_timeline(const TurnRequest& request, std::string name_space, std::uint64_t after = 0, std::size_t limit = 256);
struct TimelineReview {
    bool complete_history = false, run_exited = false, notification_issue = false;
    // 元数据本身不含固定响应及夹具，不能据此授权或重现动作。
    bool replayable = false;
    std::size_t models = 0, tools = 0, confirmations = 0, applied_effects = 0, unknown_effects = 0;
};
// 只核对值时间线的版本、关联和顺序；不接收 Session、工具工厂或执行凭证。
TimelineReview review_timeline(const RunTimeline& timeline, std::uint32_t schema_version = RunTimeline::schema_version);
// 投影当前单步骤会话；不推进工作、不读取正文，拒绝将未来多步骤快照误作单次路径。
RunTrace single_run_trace(const RunSnapshot& snapshot, std::string name_space, TraceVersions versions = {});
// 复制工具身份、结果和效果，时间暂缺时保持未知，不持有原工作或 DLL。
TraceNode tool_trace_node(const ToolRequest& request);
}
