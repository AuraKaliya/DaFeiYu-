#include "tool_verification.hpp"

namespace dfy::app {
namespace {
constexpr std::size_t state_limit = 1024;

// 核对工具已经成功且实际退出，清理故障也需要显式可见。
bool succeeded(const agent::BatchToolSnapshot& tool) noexcept {
    return tool.accepted && tool.result == runtime::OperationState::succeeded &&
        tool.exit == runtime::OperationState::succeeded && tool.fault == agent::ToolError::none;
}
}

// 建立专属观察来源及用户明确指定的状态条件。
ToolVerification::ToolVerification(std::string reader, std::string change, std::string expected, runtime::DomainId instance,
    agent::ObservationTime deadline)
    : reader_(std::move(reader)), change_(std::move(change)), expected_(runtime::Value::from_string(expected,state_limit)),
      instance_(instance), deadline_(deadline), source_(instance,agent::ObservationTrust::host_verified), inbox_({source_.id()}) {}

// 校验待审批动作紧随首个真实读取，且仍属于原提供实例。
bool ToolVerification::can_approve(const agent::ConfirmationTarget& target, agent::ObservationTime now) const noexcept {
    return !match_ && baseline_ && baseline_->current(now) && target.instance == instance_ && target.tool_id == change_ &&
        target.attempt.step.run == initial_attempt_->step.run && target.attempt.step.sequence > initial_attempt_->step.sequence;
}

// 从精确工具回执生成有界文本状态证据。
// 步骤：确认原只读回执 → 构造宿主值 → 在同一对象上接纳并返回当前观察。
agent::ObservationGuard ToolVerification::observe(const agent::TurnRequest& run, const agent::BatchToolSnapshot& tool, std::uint64_t revision,
    agent::ObservationTime now) {
    const auto actual = run.tool_result(tool.candidate);
    if (!succeeded(tool) || !actual || !actual->succeeded || actual->effect != agent::ToolEffect::not_applied)
        throw std::invalid_argument("verification: completed read-only evidence required");

    const runtime::FieldDescriptor fields[] = {{"value",runtime::ValueKind::string,state_limit}};
    auto schema = std::make_shared<const runtime::StructDescriptor>(runtime::StructDescriptor::create("dfy.app.tool_state",1,fields,{1,state_limit+128}));
    const runtime::NamedValue values[] = {{"value",runtime::Value::from_string(actual->output,state_limit)}};
    auto payload = std::make_shared<const runtime::StructValue>(runtime::StructValue::create(schema,values,state_limit+128));
    const auto receipt = inbox_.accept(source_.capture({reader_,1},revision,now,deadline_,std::move(payload)),now);
    if (receipt.status != (revision == 1 ? agent::ObservationAdmission::inserted : agent::ObservationAdmission::replaced))
        throw std::invalid_argument("verification: sample was not accepted");
    return inbox_.guard(inbox_.latest(source_.id(),reader_),now);
}

// 只以真实且按顺序退出的工具工作验证动作结果。
// 步骤：核对计划及调用顺序 → 接纳首次读取 → 在变更退出时签发反馈 → 用后续读取验证原步骤。
void ToolVerification::advance(agent::Session& session, const agent::TurnRequest& run, const agent::ConfirmationCaller& caller) {
    using agent::ObservationMatch;
    using runtime::OperationState;
    if (match_) return;
    if (run.operation()->state() != OperationState::pending || (initial_attempt_ && run.run_id() != initial_attempt_->step.run)) {
        unavailable();
        return;
    }
    const auto plan = run.plan();
    if (!plan || plan->steps.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline_) { unavailable(); return; }

    const auto steps = run.tool_steps();
    const std::string* expected_tools[] = {&reader_,&change_,&reader_};
    if (plan->steps.size() != 1 || steps.size() > 3) { unavailable(); return; }
    for (std::size_t index = 0; index < steps.size(); ++index) {
        if (steps[index].tool_id != *expected_tools[index] ||
            (index && steps[index].attempt.step.sequence <= steps[index-1].attempt.step.sequence)) { unavailable(); return; }
    }
    const auto& step = plan->steps.front();
    if (step.waiting == agent::PlanWait::verification && steps.size() != 3) { unavailable(); return; }

    if (!baseline_ && !steps.empty() && steps[0].exit == OperationState::succeeded) {
        baseline_.emplace(observe(run,steps[0],1,now));
        initial_attempt_ = steps[0].attempt;
    }
    if (!feedback_ && steps.size() >= 2 && steps[1].exit == OperationState::succeeded) {
        const auto& action = steps[1];
        if (!baseline_ || !succeeded(action) || action.effect == agent::ToolEffect::unknown ||
            action.confirmation_state != agent::ConfirmationState::consumed || steps.size() != 2) { unavailable(); return; }
        feedback_.emplace(session.request_plan_feedback(caller,step.key,*baseline_,"value",expected_,action.candidate));
    }
    if (feedback_ && step.waiting == agent::PlanWait::verification) {
        const auto current = observe(run,steps[2],2,now);
        match_ = session.verify_plan_feedback(caller,*feedback_,current).match;
        if (match_ == ObservationMatch::satisfied) baseline_.reset();
    }
}
}
