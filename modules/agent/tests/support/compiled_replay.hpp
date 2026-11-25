#pragma once
#include "replay_evidence.hpp"
#include <ostream>

namespace dfy::tests {
// 用编译标签记录采集版本，模板由原测试运行明确声明。
inline agent::RunVersionLabels compiled_replay_labels(std::string_view prompt_template) {
    agent::RunVersionLabels labels;
    if (!std::string_view(DFY_REPLAY_COMMIT).empty()) labels.application_commit = DFY_REPLAY_COMMIT;
    labels.build = DFY_REPLAY_BUILD;
    labels.model_profile = "offline-fixed-v1";
    labels.prompt_template = prompt_template;
    return labels;
}

// 将通过断言的编译案例写成独立证据，采集源仅为测试显式执行的原运行。
// 步骤：固定材料和模型计数 → 接纳值诊断 → 编码并用同一读取器复核 → 写入输出。
inline void write_compiled_replay(std::ostream& output, ScriptReport model, agent::DiagnosticSnapshot diagnostic, std::size_t tool_calls) {
    ReplayEvidence evidence{DFY_REPLAY_MATERIAL,{}};
    evidence.report.model = std::move(model);
    evidence.report.tool_calls = tool_calls;
    evidence.report.diagnostic = std::move(diagnostic);
    const auto bytes = encode_evidence(evidence);
    decode_evidence(bytes);
    output << bytes << '\n';
    if (!output) throw std::runtime_error("replay: cannot write compiled case evidence");
}

// 从不同运行独立取得诊断，再输出固定案例的原执行证据。
inline void write_compiled_replay(std::ostream& output, ScriptReport model, const agent::TurnRequest& request, std::size_t tool_calls) {
    write_compiled_replay(output,std::move(model),agent::diagnostic_snapshot(request,replay_namespace()),tool_calls);
}
}
