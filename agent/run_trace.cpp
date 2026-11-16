#include <dafeiyu/run_trace.hpp>
#include <stdexcept>
#include <limits>
#include <algorithm>

namespace dfy::agent {
namespace {
// 将同一单调时钟域的时刻转换为相对纳秒，原始时钟点不作为可恢复状态导出。
// 步骤：限定时钟精度 → 核对差值范围 → 计算相对偏移。
std::int64_t offset(TurnDiagnostics::TimePoint time, TurnDiagnostics::TimePoint origin) {
    using Duration = TurnDiagnostics::TimePoint::duration;
    static_assert(std::ratio_equal_v<Duration::period,std::nano>);
    const auto value = time.time_since_epoch().count(), base = origin.time_since_epoch().count();
    if ((base > 0 && value < std::numeric_limits<Duration::rep>::min()+base) ||
        (base < 0 && value > std::numeric_limits<Duration::rep>::max()+base)) throw std::invalid_argument("trace: time offset overflow");
    return value-base;
}
// 保留未发生时刻的未知值，不用零代替缺失时间。
std::optional<std::int64_t> optional_offset(std::optional<TurnDiagnostics::TimePoint> time, TurnDiagnostics::TimePoint origin) {
    return time ? std::optional<std::int64_t>(offset(*time,origin)) : std::nullopt;
}
// 检查诊断命名空间是有界标识，不能被当作路径或任意正文。
void validate_namespace(const std::string& value) {
    if (value.empty() || value.size() > 64) throw std::invalid_argument("trace: namespace required");
    for (const auto c : value) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) throw std::invalid_argument("trace: invalid namespace");
}
}
// 将正式单次快照转换为父子节点和事实时间线，不依赖通知是否被接纳。
// 步骤：校验身份、版本与修订 → 构造运行、步骤和尝试 → 关联真实模型状态 → 复制原事实和通知回执。
RunTrace single_run_trace(const RunSnapshot& snapshot, std::string name_space, TraceVersions versions) {
    validate_namespace(name_space); validate_trace_versions(versions);
    const auto& data = snapshot.diagnostics; const auto& attempt = data.attempt;
    if (attempt.step.run != RunId{data.session,data.turn} || attempt.step.sequence != 1 || attempt.sequence != 1 || !data.model.request ||
        data.model_requests > 1 || data.tool_requests || !snapshot.fact_count || snapshot.fact_count > snapshot.facts.size())
        throw std::invalid_argument("trace: unsupported or invalid single-run snapshot");
    const TraceNodeId root{attempt.step.run}, step{attempt.step.run,TraceNodeKind::step,1}, trial{attempt.step.run,TraceNodeKind::attempt,1,1};
    const TraceNodeId call{attempt.step.run,TraceNodeKind::model,1,1,data.model.request};
    validate_trace_id(root);
    RunTrace trace; trace.key = {std::move(name_space),root.run}; trace.versions = std::move(versions); trace.notifications = data.notifications;
    for (const auto id : {root,step,trial}) {
        TraceNode node; node.id = id; node.parent = trace_parent(id);
        if (id.kind == TraceNodeKind::run) {
            node.result = data.result; node.exit = data.exit; node.submitted_ns = 0; node.settled_ns = optional_offset(data.settled,data.model.submitted);
            node.observed_exit_ns = optional_offset(data.exited,data.model.submitted); node.accepted_ns = offset(data.accepted,data.model.submitted);
        }
        trace.nodes.push_back(std::move(node));
    }
    TraceNode model_node; model_node.id = call; model_node.parent = trial; model_node.result = data.model_result; model_node.exit = data.model_exit;
    model_node.submitted_ns = 0; model_node.call_started_ns = optional_offset(data.model.started,data.model.submitted);
    model_node.call_finished_ns = optional_offset(data.model.finished,data.model.submitted); model_node.call_outcome = data.model.outcome;
    std::array<bool,5> seen{};
    for (std::size_t index = 0; index < snapshot.fact_count; ++index) {
        const auto& fact = snapshot.facts[index]; const auto kind = static_cast<std::size_t>(fact.kind);
        if (fact.revision != index+1 || kind >= seen.size() || seen[kind]) throw std::invalid_argument("trace: invalid fact sequence");
        seen[kind] = true;
        const auto subject = fact.kind == RunFactKind::model_finished ? call : root;
        trace.events.push_back({fact.revision,subject,fact.kind,offset(fact.observed,data.model.submitted),
            fact.result,fact.cancellation,fact.model,fact.failure});
        if (fact.kind == RunFactKind::model_finished) model_node.observed_exit_ns = offset(fact.observed,data.model.submitted);
    }
    trace.nodes.push_back(std::move(model_node)); return trace;
}
// 将一次工具工作的原状态转为元数据，不复制参数、输出、原生句柄或工作持有。
TraceNode tool_trace_node(const ToolRequest& request) {
    const auto& invocation = request.invocation(); const auto& attempt = invocation.attempt;
    TraceNode node; node.id = {attempt.step.run,TraceNodeKind::tool,attempt.step.sequence,attempt.sequence,invocation.invocation};
    node.parent = trace_parent(node.id); node.result = request.operation()->state(); node.exit = request.exit()->state();
    node.effect = request.outcome().effect; node.error = request.outcome().error; node.fault = request.fault(); return node;
}
// 从权威步骤和原操作复制完整当前节点，追踪窗口只约束历史，不删改节点终态。
// 步骤：核对命名空间与版本 → 复制运行及模型节点 → 复制工具和确认摘要 → 读取有界历史增量。
RunTimeline run_timeline(const TurnRequest& request, std::string name_space, std::uint64_t after, std::size_t limit) {
    validate_namespace(name_space); const auto data = request.diagnostics(); RunTimeline result;
    result.key = {std::move(name_space),request.run_id()}; result.versions = request.trace_versions(); validate_trace_versions(result.versions);
    result.origin = data.origin; result.notifications = data.notifications; result.failure = data.failure; result.stop_reason = data.stop_reason;
    result.source = data.source; result.goal_revision = data.goal_revision; result.observations = request.observation_inputs();
    TraceNode root; root.id = {request.run_id()}; root.result = data.result; root.exit = data.exit; root.submitted_ns = 0;
    root.accepted_ns = offset(data.accepted,data.origin); root.settled_ns = optional_offset(data.settled,data.origin);
    root.observed_exit_ns = optional_offset(data.exited,data.origin); result.nodes.push_back(root);
    // 按原尝试建立无操作的分组节点；模型前被拒绝的下一尝试也保留所属层级。
    // 步骤：遍历步骤与尝试层级 → 查找已有节点 → 补齐缺失分组及父节点。
    const auto group = [&](AttemptId attempt) {
        for (const auto id : {TraceNodeId{attempt.step.run,TraceNodeKind::step,attempt.step.sequence},
            TraceNodeId{attempt.step.run,TraceNodeKind::attempt,attempt.step.sequence,attempt.sequence}}) {
            if (std::none_of(result.nodes.begin(),result.nodes.end(),[&](const auto& node) { return node.id == id; })) {
                TraceNode node; node.id = id; node.parent = trace_parent(id); result.nodes.push_back(node);
            }
        }
    };
    auto models = request.model_steps();
    if (data.model.request && std::none_of(models.begin(),models.end(),[&](const auto& model) { return model.call.request == data.model.request; }))
        models.push_back({data.attempt,data.model,data.model_result,data.model_exit});
    for (const auto& model : models) {
        group(model.attempt); TraceNode node;
        node.id = {request.run_id(),TraceNodeKind::model,model.attempt.step.sequence,model.attempt.sequence,model.call.request}; node.parent = trace_parent(node.id);
        node.result = model.result; node.exit = model.exit; node.call_outcome = model.call.outcome; node.usage = model.usage;
        node.submitted_ns = offset(model.call.submitted,data.origin); node.call_started_ns = optional_offset(model.call.started,data.origin);
        node.call_finished_ns = optional_offset(model.call.finished,data.origin);
        if (model.exit != runtime::OperationState::pending) node.observed_exit_ns = offset(model.observed_exit,data.origin);
        result.nodes.push_back(node);
    }
    for (const auto& tool : request.tool_steps()) {
        group(tool.attempt); TraceNode node;
        node.id = {request.run_id(),TraceNodeKind::tool,tool.attempt.step.sequence,tool.attempt.sequence,tool.candidate}; node.parent = trace_parent(node.id);
        node.result = tool.result; node.exit = tool.exit; node.effect = tool.effect; node.error = tool.error; node.fault = tool.fault;
        node.accepted = tool.accepted; node.related = tool.invocation; node.before = tool.before; node.after = tool.after; result.nodes.push_back(node);
    }
    for (const auto& confirmation : request.confirmation_steps()) {
        group(confirmation.attempt); TraceNode node;
        node.id = {request.run_id(),TraceNodeKind::control,confirmation.attempt.step.sequence,confirmation.attempt.sequence,confirmation.id};
        node.parent = trace_parent(node.id); node.confirmation = confirmation.state; node.related = confirmation.candidate; result.nodes.push_back(node);
    }
    result.delta = request.trace_delta(after,limit);
    for (const auto& fact : result.delta.facts) if (fact.subject.kind == TraceNodeKind::attempt)
        group({{fact.subject.run,fact.subject.step},fact.subject.attempt});
    return result;
}
}
