#pragma once
#include <dafeiyu/run_trace.hpp>

namespace dfy::agent {
// 计划的无正文步骤视图；保留验证是否存在，不导出人工证据文本、目标或完成条件。
struct PlanStepDiagnostic {
    PlanStepKey key;
    PlanStepState state = PlanStepState::pending;
    PlanWait waiting = PlanWait::none;
    std::optional<AttemptId> execution;
    std::optional<runtime::OperationState> result, exit;
    bool has_evidence = false;
};
struct PlanDiagnostic {
    PlanBinding binding;
    bool stopped = false;
    std::vector<PlanStepDiagnostic> steps;
};
// 导出准备值；时间线保留有界历史窗口，计划及反馈为当前摘要，不构成可恢复检查点。
struct DiagnosticSnapshot {
    RunTimeline timeline;
    std::vector<PlanDiagnostic> plans;
    std::vector<PlanFeedbackSnapshot> feedback;
};
// 只读取已有事实，提取无正文诊断；不执行模型、工具或反馈判定。
DiagnosticSnapshot diagnostic_snapshot(const TurnRequest& request, std::string name_space);
// 校验摘要范围和关键关联；错误只影响诊断解释，不改变原运行状态。
void validate_diagnostic_snapshot(const DiagnosticSnapshot& snapshot);
}
