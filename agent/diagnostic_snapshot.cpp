#include <dafeiyu/diagnostic_snapshot.hpp>
#include <algorithm>
#include <stdexcept>

namespace dfy::agent {
namespace {
// 报告损坏的元数据，不从缺失材料推断完成或授权。
void require(bool value) { if (!value) throw std::invalid_argument("diagnostic: inconsistent summary"); }
// 复制计划的状态和身份，定义及证据正文留在可信业务查询中。
PlanDiagnostic summarize(const PlanSnapshot& value, PlanBinding empty) {
    PlanDiagnostic result{value.definition ? value.definition->binding() : empty,value.stopped,{}};
    result.steps.reserve(value.steps.size());
    for (const auto& step : value.steps) result.steps.push_back({step.key,step.state,step.waiting,step.execution,step.result,step.exit,!step.evidence.empty()});
    return result;
}
}
// 从正式运行提取只读交付数据，所有原生工作与载荷持有都不进入返回值。
// 步骤：读取原时间线 → 汇总历史及当前计划 → 复制反馈摘要 → 校验完整元数据。
DiagnosticSnapshot diagnostic_snapshot(const TurnRequest& request, std::string name_space) {
    DiagnosticSnapshot result; result.timeline = run_timeline(request,std::move(name_space),0,1024);
    for (const auto& plan : request.plan_history()) result.plans.push_back(summarize(plan,{request.run_id(),0,0}));
    if (const auto plan = request.plan()) result.plans.push_back(summarize(*plan,{request.run_id(),request.diagnostics().goal_revision,0}));
    result.feedback = request.plan_feedback(); validate_diagnostic_snapshot(result); return result;
}
// 校验只读摘要的原运行、版本、工作及判定关联，既不解释正文，也不接收执行入口。
// 步骤：复核时间线 → 检查计划身份及状态 → 检查反馈请求和末次判定 → 拒绝跨运行或伪造完成。
void validate_diagnostic_snapshot(const DiagnosticSnapshot& value) {
    review_timeline(value.timeline); const auto run = value.timeline.key.run;
    require(value.plans.size() <= 17 && value.feedback.size() <= 64); std::uint64_t previous_goal = 0;
    for (const auto& plan : value.plans) {
        require(plan.binding.run == run && plan.binding.goal_revision > previous_goal && plan.binding.goal_revision <= value.timeline.goal_revision &&
            plan.binding.plan_revision <= 16 && plan.steps.size() <= 32 && (plan.binding.plan_revision || plan.steps.empty()));
        previous_goal = plan.binding.goal_revision;
        for (std::size_t index = 0; index < plan.steps.size(); ++index) {
            const auto& step = plan.steps[index]; require(step.key.plan.run == run && step.key.plan.goal_revision == plan.binding.goal_revision &&
                step.key.plan.plan_revision && step.key.plan.plan_revision <= plan.binding.plan_revision && !step.key.step.empty() && step.key.step.size() <= 64 &&
                static_cast<unsigned>(step.state) <= static_cast<unsigned>(PlanStepState::skipped) && static_cast<unsigned>(step.waiting) <= static_cast<unsigned>(PlanWait::stopped));
            for (std::size_t earlier = 0; earlier < index; ++earlier) require(plan.steps[earlier].key.step != step.key.step);
            require(step.execution.has_value() == step.result.has_value() && step.result.has_value() == step.exit.has_value());
            if (step.execution) require(step.execution->step.run == run && step.execution->step.sequence && step.execution->sequence &&
                step.execution->step.sequence <= value.timeline.model_contexts.size()+1 && static_cast<unsigned>(*step.result) <= 3 &&
                (*step.exit == runtime::OperationState::pending || *step.exit == runtime::OperationState::succeeded));
            if (step.state == PlanStepState::pending || step.state == PlanStepState::skipped) require(!step.execution);
            else require(step.execution.has_value());
            if (step.state == PlanStepState::completed) require(step.has_evidence && step.result == runtime::OperationState::succeeded && step.exit == runtime::OperationState::succeeded);
            if (step.state == PlanStepState::skipped) require(step.has_evidence);
            if (step.state == PlanStepState::failed) require(step.result == runtime::OperationState::failed);
            if (step.state == PlanStepState::cancelled) require(step.result == runtime::OperationState::cancelled);
        }
    }
    if (!value.plans.empty()) require(value.plans.back().binding.goal_revision == value.timeline.goal_revision);
    for (std::size_t index = 0; index < value.feedback.size(); ++index) {
        const auto& feedback = value.feedback[index]; require(feedback.request && feedback.step.plan.run == run && feedback.issued_at.step.run == run &&
            feedback.baseline.source && feedback.baseline.provider_instance && feedback.baseline.object.generation && feedback.baseline.revision &&
            !feedback.baseline.object.id.empty() && feedback.baseline.object.id.size() <= 128 && feedback.latest.has_value() == (feedback.checks != 0));
        require(feedback.expected_type == runtime::ValueKind::boolean || feedback.expected_type == runtime::ValueKind::int64 ||
            feedback.expected_type == runtime::ValueKind::uint64 || feedback.expected_type == runtime::ValueKind::string || feedback.expected_type == runtime::ValueKind::null_value);
        for (std::size_t earlier = 0; earlier < index; ++earlier) require(value.feedback[earlier].request != feedback.request);
        const auto context = std::find_if(value.timeline.model_contexts.begin(),value.timeline.model_contexts.end(),[&](const auto& item) { return item.attempt == feedback.issued_at; });
        require(context != value.timeline.model_contexts.end() && context->purpose == ModelPurpose::plan_step && context->goal_revision == feedback.step.plan.goal_revision &&
            context->plan_revision == feedback.step.plan.plan_revision && context->plan_step == feedback.step.step);
        if (feedback.completed_tool) require(std::any_of(value.timeline.nodes.begin(),value.timeline.nodes.end(),[&](const auto& node) {
            return node.id.kind == TraceNodeKind::tool && node.id.local == feedback.completed_tool && node.accepted == true && node.exit == runtime::OperationState::succeeded;
        }));
        if (feedback.latest) require(static_cast<unsigned>(feedback.latest->match) <= static_cast<unsigned>(ObservationMatch::unknown) &&
            static_cast<unsigned>(feedback.latest->issue) <= static_cast<unsigned>(ObservationEvidenceIssue::type_changed) &&
            feedback.verified == (feedback.latest->match == ObservationMatch::satisfied) &&
            (feedback.latest->match == ObservationMatch::unknown ? feedback.latest->issue != ObservationEvidenceIssue::none :
                feedback.latest->issue == ObservationEvidenceIssue::none && feedback.latest->source == feedback.baseline.source && feedback.latest->revision > feedback.baseline.revision));
        else require(!feedback.verified);
        if (feedback.verified) require(feedback.baseline.trust == ObservationTrust::host_verified && std::any_of(value.plans.begin(),value.plans.end(),[&](const auto& plan) {
            return std::any_of(plan.steps.begin(),plan.steps.end(),[&](const auto& step) { return step.key == feedback.step && step.state == PlanStepState::completed && step.has_evidence; });
        }));
    }
}
}
