#include <dafeiyu/run_trace.hpp>
#include <algorithm>
#include <stdexcept>

namespace dfy::agent {
namespace {
// 拒绝损坏或互相矛盾的诊断数据，不猜补未保留事实。
void require(bool condition) { if (!condition) throw std::invalid_argument("trace review: inconsistent metadata"); }
// 核对值枚举的定义范围，不将未知格式解释为默认行为。
template<class T> bool within(T value, T maximum) { return static_cast<unsigned>(value) <= static_cast<unsigned>(maximum); }
// 检查可选状态，缺失表示该节点没有对应原操作。
void operation(const std::optional<runtime::OperationState>& value) {
    if (value) require(within(*value,runtime::OperationState::cancelled));
}
// 只比较已知的同域相对时刻，缺失信息保持未知而不是补成零。
void ordered(std::optional<std::int64_t> earlier, std::optional<std::int64_t> later) {
    if (earlier && later) require(*earlier <= *later);
}
// 定位完整节点身份；未找到返回空，不以相似编号跨节点绑定。
const TraceNode* find_node(const RunTimeline& value, TraceNodeId id) {
    const auto found = std::find_if(value.nodes.begin(),value.nodes.end(),[&](const auto& node) { return node.id == id; });
    return found == value.nodes.end() ? nullptr : &*found;
}
// 核对明确控制决定的合法组合，不执行任何控制回调。
void decision(PolicyDecision value) {
    require(within(value.reason,PolicyReason::policy_failed));
    if (value.verdict == PolicyVerdict::allow) require(value.reason == PolicyReason::none);
    else if (value.verdict == PolicyVerdict::wait) require(value.reason == PolicyReason::confirmation_required || value.reason == PolicyReason::work_pending);
    else require(value.verdict == PolicyVerdict::deny && value.reason != PolicyReason::none &&
        value.reason != PolicyReason::confirmation_required && value.reason != PolicyReason::work_pending);
}
}
// 只读解释已有元数据，损坏拒绝、缺口明示，元数据不能变成当前执行许可。
// 步骤：核对格式和大小 → 验证完整节点及关联 → 验证保留窗口和单调事实 → 汇总可见状态及缺失材料。
TimelineReview review_timeline(const RunTimeline& value, std::uint32_t schema_version) {
    require(schema_version == RunTimeline::schema_version && !value.nodes.empty() && value.nodes.size() <= 512 && value.delta.facts.size() <= 1024);
    require(!value.key.name_space.empty() && value.key.name_space.size() <= 64);
    for (const auto c : value.key.name_space) require((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.');
    validate_trace_versions(value.versions); validate_trace_id({value.key.run});
    require(value.goal_revision && within(value.source,RunOrigin::observation_rule) && value.observations.size() <= 4);
    for (const auto& observation : value.observations) require(observation.source && observation.provider_instance && observation.object.generation &&
        !observation.object.id.empty() && observation.object.id.size() <= 128 && observation.revision && within(observation.trust,ObservationTrust::host_verified));
    require(within(value.failure,TurnFailure::model_request) && within(value.stop_reason,PolicyReason::policy_failed));
    const auto root = find_node(value,{value.key.run}); require(root && root->result && root->exit);
    TimelineReview report; report.run_exited = root->exit == runtime::OperationState::succeeded;
    require(!report.run_exited || root->result != runtime::OperationState::pending);
    for (std::size_t index = 0; index < value.nodes.size(); ++index) {
        const auto& node = value.nodes[index]; validate_trace_id(node.id); require(node.id.run == value.key.run && node.parent == trace_parent(node.id));
        require(std::none_of(value.nodes.begin(),value.nodes.begin()+index,[&](const auto& earlier) { return earlier.id == node.id; }));
        if (node.parent) require(find_node(value,*node.parent));
        operation(node.result); operation(node.exit);
        ordered(node.submitted_ns,node.accepted_ns); ordered(node.submitted_ns,node.call_started_ns);
        ordered(node.call_started_ns,node.call_finished_ns); ordered(node.call_finished_ns,node.observed_exit_ns);
        ordered(node.settled_ns,node.observed_exit_ns);
        require(!node.exit || *node.exit == runtime::OperationState::pending || *node.exit == runtime::OperationState::succeeded);
        if (node.before) decision(*node.before); if (node.after) decision(*node.after);
        if (node.effect) require(within(*node.effect,ToolEffect::unknown));
        require(within(node.error,ToolError::deadline) && within(node.fault,ToolError::deadline));
        if (node.id.kind == TraceNodeKind::step || node.id.kind == TraceNodeKind::attempt) require(!node.result && !node.exit);
        if (node.id.kind == TraceNodeKind::model) {
            ++report.models; require(node.result && node.exit && node.call_outcome && within(*node.call_outcome,model::CallOutcome::threw));
        } else if (node.id.kind == TraceNodeKind::tool) {
            ++report.tools; require(node.result && node.exit && node.accepted && node.effect && (node.related != 0) == *node.accepted);
            if (*node.effect == ToolEffect::applied) { require(*node.accepted); ++report.applied_effects; }
            if (*node.effect == ToolEffect::unknown) { require(*node.accepted); ++report.unknown_effects; }
        } else if (node.id.kind == TraceNodeKind::control) {
            ++report.confirmations; require(node.confirmation && within(*node.confirmation,ConfirmationState::consumed));
            require(find_node(value,{node.id.run,TraceNodeKind::tool,node.id.step,node.id.attempt,node.related}));
            if (report.run_exited) require(*node.confirmation != ConfirmationState::waiting && *node.confirmation != ConfirmationState::approved);
        }
    }
    const auto& delta = value.delta;
    require(delta.run == value.key.run && delta.latest_revision && delta.oldest_revision && delta.oldest_revision-1 <= delta.latest_revision &&
        delta.latest_revision-(delta.oldest_revision-1) <= 1024 && delta.requested_after <= delta.latest_revision &&
        delta.gap == (delta.requested_after < delta.oldest_revision-1));
    auto previous = std::max(delta.requested_after,delta.oldest_revision-1);
    std::array<bool,static_cast<std::size_t>(TraceFactKind::exited)+1> root_facts{};
    std::size_t history_messages = 0;
    for (std::size_t index = 0; index < delta.facts.size(); ++index) {
        const auto& fact = delta.facts[index]; require(previous < delta.latest_revision && fact.revision == previous+1); previous = fact.revision;
        const auto node = find_node(value,fact.subject);
        require(node && within(fact.kind,TraceFactKind::exited) && within(fact.cancellation,runtime::CancelReason::deadline));
        require(within(fact.result,runtime::OperationState::cancelled) && within(fact.exit,runtime::OperationState::cancelled) &&
            within(fact.stop_reason,PolicyReason::policy_failed) && within(fact.error,ToolError::deadline) && within(fact.fault,ToolError::deadline));
        if (fact.effect) require(within(*fact.effect,ToolEffect::unknown));
        if (fact.model_outcome) require(within(*fact.model_outcome,model::CallOutcome::threw));
        if (fact.control_point) require(within(*fact.control_point,ControlPoint::tool_result));
        if (fact.decision) decision(*fact.decision);
        if (fact.confirmation) require(within(*fact.confirmation,ConfirmationState::consumed));
        switch (fact.kind) {
        case TraceFactKind::model_submitted: case TraceFactKind::model_exited:
            require(fact.subject.kind == TraceNodeKind::model);
            if (fact.kind == TraceFactKind::model_exited) require(fact.exit == runtime::OperationState::succeeded &&
                node->result == fact.result && node->exit == fact.exit && node->call_outcome == fact.model_outcome);
            break;
        case TraceFactKind::tool_candidate: case TraceFactKind::tool_changed:
            require(fact.subject.kind == TraceNodeKind::tool && fact.effect);
            if (fact.exit == runtime::OperationState::succeeded) require(node->result == fact.result && node->exit == fact.exit &&
                node->effect == fact.effect && node->error == fact.error && node->fault == fact.fault);
            break;
        case TraceFactKind::confirmation_changed:
            require(fact.subject.kind == TraceNodeKind::control && fact.confirmation && fact.source_revision && node->related == fact.related);
            break;
        case TraceFactKind::policy_decision:
            require(fact.decision && fact.control_point);
            require(fact.subject.kind == (*fact.control_point == ControlPoint::model_request ? TraceNodeKind::attempt : TraceNodeKind::tool));
            break;
        case TraceFactKind::history_committed:
            require(fact.subject.kind == TraceNodeKind::attempt && fact.history_messages > history_messages && fact.history_messages <= 1024);
            history_messages = fact.history_messages; break;
        default:
            require(fact.subject.kind == TraceNodeKind::run && !root_facts[static_cast<std::size_t>(fact.kind)]);
            root_facts[static_cast<std::size_t>(fact.kind)] = true;
            if (fact.kind == TraceFactKind::settled || fact.kind == TraceFactKind::exited) require(node->result == fact.result);
            if (fact.kind == TraceFactKind::exited) require(node->exit == fact.exit && fact.exit == runtime::OperationState::succeeded);
        }
        if (index) require(fact.observed >= delta.facts[index-1].observed);
        if (fact.source_revision) for (std::size_t earlier = 0; earlier < index; ++earlier) {
            const auto& old = delta.facts[earlier];
            if (old.subject == fact.subject && old.source_revision) require(old.source_revision < fact.source_revision);
        }
    }
    require(delta.more == (previous < delta.latest_revision) && (!delta.more || !delta.facts.empty()));
    report.complete_history = !delta.requested_after && !delta.gap && !delta.more && !delta.projection_failed;
    if (report.complete_history) require(root_facts[static_cast<std::size_t>(TraceFactKind::accepted)] &&
        (!report.run_exited || root_facts[static_cast<std::size_t>(TraceFactKind::exited)]));
    for (const auto& receipt : {value.notifications.started,value.notifications.finished})
        if (receipt && receipt->status != events::ObservationStatus::accepted) report.notification_issue = true;
    return report;
}
}
