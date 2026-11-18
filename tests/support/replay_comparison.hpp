#pragma once
#include "replay_material.hpp"
#include <dafeiyu/digest.hpp>
#include <random>
#include <span>
#include <tuple>

namespace dfy::tests {
struct ReplayEvidence {
    std::string input_digest;
    ReplayReport report;
};
enum class ReplayComparisonState { equivalent, differs, incomparable };
struct ReplayComparison {
    ReplayComparisonState state = ReplayComparisonState::incomparable;
    std::string reason;
    agent::VersionDifference versions;
    std::vector<std::string_view> behavior;
};

// 为独立执行分配跨进程可区分的诊断命名空间。
inline std::string replay_namespace() {
    std::random_device source;
    std::string name = "offline-replay";
    for (unsigned i = 0; i < 4; ++i) name += "-"+std::to_string(source());
    return name;
}

// 从严格固定的材料取得实际执行报告，策略字段由实际运行版本独立证明。
// 步骤：解析并校验场景 → 固定其余输入与原预期的摘要 → 在原执行器中测量。
inline ReplayEvidence measure_material(std::string_view text, agent::RunVersionLabels version_labels) {
    auto value = material::parse(text);
    auto scenario = parse_replay_value(value);
    value.erase("policy");
    value.erase("timeout_ms");
    auto input_digest = platform::sha256(value.dump());
    auto report = measure_replay(std::move(scenario),std::move(version_labels));
    report.diagnostic.timeline.key.name_space = replay_namespace();
    return {std::move(input_digest),std::move(report)};
}

// 保留节点的相对层级和业务状态，绝对身份及耗时已由回顾单独校验。
inline auto replay_node_values(const agent::TraceNode& node) {
    return std::tie(node.id.kind,node.id.step,node.id.attempt,node.result,node.exit,node.call_outcome,node.effect,
        node.error,node.fault,node.accepted,node.before,node.after,node.confirmation,node.tool_id);
}

// 用已核对的节点位置保持事实与原节点的关联。
inline std::ptrdiff_t replay_node_position(const agent::RunTimeline& timeline, const agent::TraceNodeId& id) {
    return std::find_if(timeline.nodes.begin(),timeline.nodes.end(),[&](const auto& node) { return node.id == id; })-timeline.nodes.begin();
}

// 确认关系按同一次尝试的原候选位置对齐；其他节点的归属由节点身份校验。
inline std::ptrdiff_t replay_confirmation_target(const agent::RunTimeline& timeline, const agent::TraceNode& node) {
    if (node.id.kind != agent::TraceNodeKind::control) return -1;
    return replay_node_position(timeline,{node.id.run,agent::TraceNodeKind::tool,node.id.step,node.id.attempt,node.related});
}

// 保留有序的结算、效果、控制和上下文事实，不把宿主观察时刻当作行为差异。
inline auto replay_fact_values(const agent::TraceFact& fact) {
    return std::tie(fact.kind,fact.result,fact.exit,fact.cancellation,fact.stop_reason,fact.model_outcome,fact.effect,
        fact.error,fact.fault,fact.control_point,fact.decision,fact.confirmation,fact.source_revision,fact.history_messages);
}

// 比较每个原模型调用使用的目标、计划及观察复核结果，绑定身份由各自的时间线校验。
inline auto replay_model_context_values(const agent::ModelContextSnapshot& context) {
    return std::tie(context.attempt.step.sequence,context.attempt.sequence,context.goal_revision,context.plan_revision,
        context.purpose,context.plan_step,context.observations_current);
}

// 保留计划步骤的相对版本与业务标识，原运行归属已经由诊断校验。
inline auto replay_plan_key(const agent::PlanStepKey& key) {
    return std::tie(key.plan.goal_revision,key.plan.plan_revision,key.step);
}

// 提取原模型或工具工作在当前运行内的相对位置。
inline auto replay_attempt_key(const agent::AttemptId& attempt) {
    return std::tie(attempt.step.sequence,attempt.sequence);
}

// 比较计划及原步骤的结果、实际退出和宿主证据，不读取定义或证据正文。
// 步骤：核对计划版本与停止状态 → 按顺序匹配步骤 → 比较原工作关联。
inline bool replay_plan_equal(const agent::PlanDiagnostic& left, const agent::PlanDiagnostic& right) {
    if (std::tie(left.binding.goal_revision,left.binding.plan_revision,left.stopped) !=
        std::tie(right.binding.goal_revision,right.binding.plan_revision,right.stopped)) return false;
    return std::equal(left.steps.begin(),left.steps.end(),right.steps.begin(),right.steps.end(),[](const auto& a,const auto& b) {
        return replay_plan_key(a.key) == replay_plan_key(b.key) &&
            std::tie(a.state,a.waiting,a.result,a.exit,a.has_evidence) == std::tie(b.state,b.waiting,b.result,b.exit,b.has_evidence) &&
            a.execution.has_value() == b.execution.has_value() &&
            (!a.execution || replay_attempt_key(*a.execution) == replay_attempt_key(*b.execution));
    });
}

// 保留原观察对象、代次、修订和信任级别，来源编号按运行内关系比较。
inline auto replay_observation_values(const agent::ObservationInputFact& value) {
    return std::tie(value.object.id,value.object.generation,value.revision,value.trust);
}

// 比较有序观察及同源、同实例关系，不要求两次执行分配相同进程内编号。
// 步骤：核对集合大小和值 → 按前序位置核对来源及实例的复用关系。
inline bool replay_observations_equal(const std::vector<agent::ObservationInputFact>& left, const std::vector<agent::ObservationInputFact>& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto& a = left[index]; const auto& b = right[index];
        if (replay_observation_values(a) != replay_observation_values(b)) return false;
        for (std::size_t prior = 0; prior < index; ++prior) {
            if ((a.source == left[prior].source) != (b.source == right[prior].source) ||
                (a.provider_instance == left[prior].provider_instance) != (b.provider_instance == right[prior].provider_instance)) return false;
        }
    }
    return true;
}

// 比较替换发生的原步骤及模型边界，绑定链的原始身份已由时间线验证。
inline bool replay_rebind_equal(const agent::ObservationRebindReceipt& left, const agent::ObservationRebindReceipt& right) {
    return replay_plan_key(left.step) == replay_plan_key(right.step) && left.after_model_requests == right.after_model_requests &&
        replay_observations_equal(left.observations,right.observations);
}

// 从诊断找到反馈签发时实际引用的工具位置，零表示引用整个步骤。
inline std::ptrdiff_t replay_feedback_tool(const agent::RunTimeline& trace, runtime::DomainId tool) {
    if (!tool) return -1;
    return std::find_if(trace.nodes.begin(),trace.nodes.end(),[tool](const auto& node) {
        return node.id.kind == agent::TraceNodeKind::tool && node.id.local == tool;
    })-trace.nodes.begin();
}

// 将反馈基线对应到原观察集合，独立的读回来源保留为集合外。
inline std::ptrdiff_t replay_feedback_observation(const agent::RunTimeline& trace, const agent::ObservationInputFact& basis) {
    return std::find_if(trace.observations.begin(),trace.observations.end(),[&](const auto& item) {
        return item.source == basis.source && item.provider_instance == basis.provider_instance &&
            item.object.id == basis.object.id && item.object.generation == basis.object.generation;
    })-trace.observations.begin();
}

// 比较原工具和基线、检查次数及实际反馈结算，不把随机请求号当作行为。
// 步骤：配对原步骤、工具及基线 → 比较检查和结果 → 校验末次采样的相对来源。
inline bool replay_feedback_equal(const agent::PlanFeedbackSnapshot& a, const agent::RunTimeline& left,
    const agent::PlanFeedbackSnapshot& b, const agent::RunTimeline& right) {
    if (replay_plan_key(a.step) != replay_plan_key(b.step) || replay_attempt_key(a.issued_at) != replay_attempt_key(b.issued_at) ||
        replay_feedback_tool(left,a.completed_tool) != replay_feedback_tool(right,b.completed_tool) ||
        replay_observation_values(a.baseline) != replay_observation_values(b.baseline) ||
        replay_feedback_observation(left,a.baseline) != replay_feedback_observation(right,b.baseline) ||
        std::tie(a.expected_type,a.checks,a.verified,a.checks_saturated) != std::tie(b.expected_type,b.checks,b.verified,b.checks_saturated) ||
        a.latest.has_value() != b.latest.has_value()) return false;
    if (!a.latest) return true;
    const auto& x = *a.latest; const auto& y = *b.latest;
    return std::tie(x.match,x.issue,x.revision) == std::tie(y.match,y.issue,y.revision) &&
        (x.source == a.baseline.source) == (y.source == b.baseline.source) && static_cast<bool>(x.source) == static_cast<bool>(y.source);
}

// 按同案例和已声明变化比较两次实际执行，缺材料或缺事实时明确不可比。
// 步骤：核对案例输入 → 核对版本及声明 → 校验历史和身份 → 比较结构化行为。
inline ReplayComparison compare_replays(const ReplayEvidence& baseline, const ReplayEvidence& candidate,
    std::span<const std::string_view> declared = {}) {
    const auto& left = baseline.report;
    const auto& right = candidate.report;
    const auto& left_trace = left.diagnostic.timeline;
    const auto& right_trace = right.diagnostic.timeline;
    ReplayComparison result;
    if (left.model.case_id != right.model.case_id || left.model.case_version != right.model.case_version ||
        baseline.input_digest.empty() || baseline.input_digest != candidate.input_digest) {
        result.reason = "case inputs differ";
        return result;
    }
    result.versions = agent::compare_public_versions(left_trace.versions,right_trace.versions);
    // 此执行入口只使用内存工具夹具，因此没有可比较的原生插件版本。
    std::erase_if(result.versions.unknown,[](const auto& name) { return name.starts_with("tool/fixture/") && name.ends_with("/plugin"); });
    if (!result.versions.unknown.empty()) { result.reason = "required versions are unknown"; return result; }
    for (const auto& name : result.versions.changed) if (std::find(declared.begin(),declared.end(),name) == declared.end()) {
        result.reason = "undeclared version change";
        return result;
    }

    const auto first = agent::review_timeline(left_trace), second = agent::review_timeline(right_trace);
    if (left_trace.key == right_trace.key || !first.complete_history || !second.complete_history || !first.run_exited || !second.run_exited) {
        result.reason = "independent complete histories required";
        return result;
    }

    const auto difference = [&](std::string_view name, const auto& a, const auto& b) {
        if (a != b) result.behavior.push_back(name);
    };
    difference("model_requests",left.model.attempted,right.model.attempted);
    difference("tool_calls",left.tool_calls,right.tool_calls);
    difference("tool_effects",left.effects,right.effects);
    difference("history_messages",left.history_messages,right.history_messages);
    difference("stop_reason",left_trace.stop_reason,right_trace.stop_reason);
    difference("failure",left_trace.failure,right_trace.failure);
    difference("run_origin",left_trace.source,right_trace.source);
    difference("goal_revision",left_trace.goal_revision,right_trace.goal_revision);
    difference("notification_issue",first.notification_issue,second.notification_issue);
    difference("assertions",left.failed_assertions,right.failed_assertions);
    if (static_cast<bool>(left_trace.initial_observation_binding) != static_cast<bool>(right_trace.initial_observation_binding) ||
        !replay_observations_equal(left_trace.observations,right_trace.observations)) result.behavior.push_back("observations");
    if (!std::equal(left_trace.observation_rebinds.begin(),left_trace.observation_rebinds.end(),
        right_trace.observation_rebinds.begin(),right_trace.observation_rebinds.end(),replay_rebind_equal))
        result.behavior.push_back("observation_rebinds");
    if (!std::equal(left_trace.model_contexts.begin(),left_trace.model_contexts.end(),
        right_trace.model_contexts.begin(),right_trace.model_contexts.end(),
        [](const auto& a,const auto& b) { return replay_model_context_values(a) == replay_model_context_values(b); }))
        result.behavior.push_back("model_contexts");
    if (!std::equal(left.diagnostic.plans.begin(),left.diagnostic.plans.end(),
        right.diagnostic.plans.begin(),right.diagnostic.plans.end(),replay_plan_equal)) result.behavior.push_back("plans");
    if (!std::equal(left.diagnostic.feedback.begin(),left.diagnostic.feedback.end(),
        right.diagnostic.feedback.begin(),right.diagnostic.feedback.end(),
        [&](const auto& a,const auto& b) { return replay_feedback_equal(a,left_trace,b,right_trace); })) result.behavior.push_back("feedback");
    if (!std::equal(left_trace.nodes.begin(),left_trace.nodes.end(),right_trace.nodes.begin(),right_trace.nodes.end(),
        [&](const auto& a,const auto& b) {
            return replay_node_values(a) == replay_node_values(b) &&
                replay_confirmation_target(left_trace,a) == replay_confirmation_target(right_trace,b);
        })) result.behavior.push_back("nodes");
    if (!std::equal(left_trace.delta.facts.begin(),left_trace.delta.facts.end(),right_trace.delta.facts.begin(),right_trace.delta.facts.end(),
        [&](const auto& a,const auto& b) {
            return replay_fact_values(a) == replay_fact_values(b) &&
                replay_node_position(left_trace,a.subject) == replay_node_position(right_trace,b.subject);
        })) result.behavior.push_back("fact_order");
    result.state = result.behavior.empty() ? ReplayComparisonState::equivalent : ReplayComparisonState::differs;
    return result;
}
}
