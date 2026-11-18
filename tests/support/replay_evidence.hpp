#pragma once
#include "replay_comparison.hpp"
#include <dafeiyu/diagnostic_json.hpp>
#include <array>

namespace dfy::tests {
inline constexpr std::size_t evidence_limit = 2*1024*1024;

// 编码固定场景的无正文执行证据，复用严格诊断时间线格式。
// 步骤：编码原运行值 → 增加案例摘要和预算内断言统计 → 核对总大小。
inline std::string encode_evidence(const ReplayEvidence& evidence) {
    const auto& report = evidence.report;
    const auto timeline = agent::encode_diagnostic({report.trace,{},{}});
    auto text = material::Json{{"schema_version",1},{"input_digest",evidence.input_digest},
        {"case_id",report.model.case_id},{"case_version",report.model.case_version},{"model_calls",report.model.attempted},
        {"tool_calls",report.tool_calls},{"failed_assertions",report.failed_assertions},{"diagnostic",material::Json::parse(timeline)}}.dump();
    if (text.size() > evidence_limit) throw std::length_error("evidence: report limit exceeded");
    return text;
}

// 严格读取另一构建的固定执行证据，只接受完整的值记录。
// 步骤：校验案例及版本 → 还原原时间线 → 核对调用及断言统计 → 从原事实提取效果和历史。
inline ReplayEvidence decode_evidence(std::string_view text) {
    using namespace material;
    constexpr std::string_view names[] = {"result","stop_reason","tool_calls","effects","history_messages"};
    const auto value = parse(text);
    keys(value,{"schema_version","input_digest","case_id","case_version","model_calls","tool_calls","failed_assertions","diagnostic"});
    if (number(value.at("schema_version"),1) != 1) throw std::invalid_argument("evidence: unsupported report version");
    ReplayEvidence evidence;
    auto& report = evidence.report;
    evidence.input_digest = value.at("input_digest").get<std::string>();
    report.model.case_id = value.at("case_id").get<std::string>();
    report.model.case_version = static_cast<std::uint32_t>(number(value.at("case_version"),UINT32_MAX));
    if (evidence.input_digest.size() != 64 || evidence.input_digest.find_first_not_of("0123456789abcdef") != std::string::npos ||
        report.model.case_id.empty() || report.model.case_id.size() > 64 ||
        report.model.case_id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") != std::string::npos ||
        !report.model.case_version) throw std::invalid_argument("evidence: invalid case identity");

    auto diagnostic = agent::decode_diagnostic(value.at("diagnostic").dump());
    report.trace = std::move(diagnostic.timeline);
    const auto review = agent::review_timeline(report.trace);
    report.model.attempted = report.model.consumed = static_cast<std::size_t>(number(value.at("model_calls"),64));
    report.tool_calls = static_cast<std::size_t>(number(value.at("tool_calls"),64));
    if (!diagnostic.plans.empty() || !diagnostic.feedback.empty() || !review.complete_history || !review.run_exited ||
        report.trace.versions.model_profile != "offline-fixed-v1" || review.models != report.model.attempted || review.tools < report.tool_calls)
        throw std::invalid_argument("evidence: report and execution facts disagree");
    for (const auto& item : array(value.at("failed_assertions"),std::size(names))) {
        const auto name = item.get<std::string>();
        const auto known = std::find(std::begin(names),std::end(names),name);
        if (known == std::end(names) || std::find(report.failed_assertions.begin(),report.failed_assertions.end(),*known) != report.failed_assertions.end())
            throw std::invalid_argument("evidence: invalid assertion");
        report.failed_assertions.push_back(*known);
    }
    for (const auto& node : report.trace.nodes) if (node.id.kind == agent::TraceNodeKind::tool) report.effects.push_back(node.effect.value());
    for (const auto& fact : report.trace.delta.facts)
        if (fact.kind == agent::TraceFactKind::history_committed) report.history_messages = fact.history_messages;
    return evidence;
}
}
