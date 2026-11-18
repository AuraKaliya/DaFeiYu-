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

// 从严格固定的材料取得实际执行报告，策略字段由实际运行版本独立证明。
// 步骤：解析并校验场景 → 固定其余输入与原预期的摘要 → 在原执行器中测量。
inline ReplayEvidence measure_material(std::string_view text, agent::RunVersionLabels version_labels) {
    auto value = material::parse(text);
    auto scenario = parse_replay_value(value);
    value.erase("policy");
    value.erase("timeout_ms");
    auto input_digest = platform::sha256(value.dump());
    auto report = measure_replay(std::move(scenario),std::move(version_labels));
    std::random_device source;
    for (unsigned i = 0; i < 4; ++i) report.trace.key.name_space += "-"+std::to_string(source());
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

// 保留有序的结算、效果、控制和上下文事实，不把宿主观察时刻当作行为差异。
inline auto replay_fact_values(const agent::TraceFact& fact) {
    return std::tie(fact.kind,fact.result,fact.exit,fact.cancellation,fact.stop_reason,fact.model_outcome,fact.effect,
        fact.error,fact.fault,fact.control_point,fact.decision,fact.confirmation,fact.source_revision,fact.history_messages);
}

// 按同案例和已声明变化比较两次实际执行，缺材料或缺事实时明确不可比。
// 步骤：核对案例输入 → 核对版本及声明 → 校验历史和身份 → 比较结构化行为。
inline ReplayComparison compare_replays(const ReplayEvidence& baseline, const ReplayEvidence& candidate,
    std::span<const std::string_view> declared = {}) {
    const auto& left = baseline.report;
    const auto& right = candidate.report;
    ReplayComparison result;
    if (left.model.case_id != right.model.case_id || left.model.case_version != right.model.case_version ||
        baseline.input_digest.empty() || baseline.input_digest != candidate.input_digest) {
        result.reason = "case inputs differ";
        return result;
    }
    result.versions = agent::compare_public_versions(left.trace.versions,right.trace.versions);
    // 此执行入口只使用内存工具夹具，因此没有可比较的原生插件版本。
    std::erase_if(result.versions.unknown,[](const auto& name) { return name.starts_with("tool/fixture/") && name.ends_with("/plugin"); });
    if (!result.versions.unknown.empty()) { result.reason = "required versions are unknown"; return result; }
    for (const auto& name : result.versions.changed) if (std::find(declared.begin(),declared.end(),name) == declared.end()) {
        result.reason = "undeclared version change";
        return result;
    }

    const auto first = agent::review_timeline(left.trace), second = agent::review_timeline(right.trace);
    if (left.trace.key == right.trace.key || !first.complete_history || !second.complete_history || !first.run_exited || !second.run_exited) {
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
    difference("stop_reason",left.trace.stop_reason,right.trace.stop_reason);
    difference("failure",left.trace.failure,right.trace.failure);
    difference("notification_issue",first.notification_issue,second.notification_issue);
    difference("assertions",left.failed_assertions,right.failed_assertions);
    if (!std::equal(left.trace.nodes.begin(),left.trace.nodes.end(),right.trace.nodes.begin(),right.trace.nodes.end(),
        [](const auto& a,const auto& b) { return replay_node_values(a) == replay_node_values(b); })) result.behavior.push_back("nodes");
    if (!std::equal(left.trace.delta.facts.begin(),left.trace.delta.facts.end(),right.trace.delta.facts.begin(),right.trace.delta.facts.end(),
        [&](const auto& a,const auto& b) {
            return replay_fact_values(a) == replay_fact_values(b) &&
                replay_node_position(left.trace,a.subject) == replay_node_position(right.trace,b.subject);
        })) result.behavior.push_back("fact_order");
    result.state = result.behavior.empty() ? ReplayComparisonState::equivalent : ReplayComparisonState::differs;
    return result;
}
}
