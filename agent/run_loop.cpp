#include <dafeiyu/run_loop.hpp>
#include <dafeiyu/plan_protocol.hpp>
#include <dafeiyu/session.hpp>
#include <dafeiyu/utf8.hpp>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace dfy::agent {
namespace {
RunLoopState* retained_head = nullptr;
std::size_t resident_count = 0, retained_count = 0;
bool reaping = false;
// 原运行共享的行动依据，工具只捕获此小型状态；更新不能重建执行器或清除旧回执。
struct ActionBasis { std::optional<ActionObservations> observations; bool available = true; };
// 由原 RunLoop 拥有一个计划步骤的工作完成源；计划账本只借用读句柄，验证状态仍独立。
struct PlanExecution {
    PlanStepKey key;
    std::size_t index;
    runtime::OperationSource result, exit;
    std::string answer;
    // 在任何模型或工具调用之前预留本步骤的独立结果与退出操作。
    PlanExecution(PlanStepKey value, std::size_t position) : key(std::move(value)),index(position) {}
};
}
struct RunLoopState {
    std::shared_ptr<TurnRequest> record;
    model::Config config;
    model::AsyncClient models;
    RunOptions options;
    std::unique_ptr<ExecutionPolicy> policy;
    ToolTime deadline;
    ToolBatch* batch = nullptr;
    std::optional<PlanScope> planning_scope;
    std::optional<PlanPrompt> planning_request;
    std::string planning_goal;
    std::optional<ToolSnapshot> step_tools;
    std::unique_ptr<PlanExecution> plan_step;
    std::shared_ptr<ActionBasis> action_basis = std::make_shared<ActionBasis>();
    std::size_t repeated = 0;
    std::optional<std::uint64_t> last_fingerprint;
    TurnFailure stage = TurnFailure::model_request;
    RunLoopState* next = nullptr;
    bool advancing = false;
    // 在首个模型调用之前固定运行资源，全部分配完成后由外层启动。
    // 步骤：创建原回合与模型入口 → 校验选项和显式规划 → 预留步骤及上下文 → 准备工具执行器。
    RunLoopState(RunId run, model::Config value, model::Backend backend, std::shared_ptr<const MessageHistory> history,
        std::string prompt, RunOptions settings)
        : record(new TurnRequest(run,settings.trace_capacity)), config(std::move(value)), models(1,std::move(backend)), options(std::move(settings)),
          policy(std::make_unique<ExecutionPolicy>(run,options.goal_revision,options.policy)) {
        if (!options.now || options.timeout <= std::chrono::milliseconds::zero() || options.timeout > std::chrono::hours(1) ||
            !options.no_progress_limit || options.no_progress_limit > 64 || (options.planning && (!options.goal_update_limit || options.goal_update_limit > 16 ||
                !options.observation_rebind_limit || options.observation_rebind_limit > 64)))
            throw std::invalid_argument("run: invalid limits");
        const auto now = options.now(); const auto duration = std::chrono::duration_cast<ToolTime::duration>(options.timeout);
        if (now > ToolTime::max()-duration) throw std::overflow_error("run: deadline overflow");
        deadline = std::min(now+duration,options.deadline.value_or(now+duration));
        if (options.action_observations) {
            const auto guarded = options.action_observations->observations();
            if (!options.action_observations->current(now) || (!options.observations.empty() && options.observations != guarded))
                throw std::invalid_argument("run: mismatched or stale action observations");
            options.observations = guarded;
            record->initial_observation_binding_ = options.action_observations->binding();
        }
        const bool changes = options.tools && options.tools->permits_state_changes();
        const bool observed_change = changes && options.origin == RunOrigin::observation_rule;
        if (observed_change != options.observation_action.has_value() ||
            (observed_change && (!options.observation_action->matches(record->initial_observation_binding_) || options.planning ||
                !options.confirmation_caller || !options.policy.confirm_changes || options.policy.tool_limit != 1 ||
                options.tools->descriptions(false).size() != 1)))
            throw std::invalid_argument("run: bound observation action required");
        if (changes && !options.observations.empty() && !options.action_observations)
            throw std::invalid_argument("run: observed changes require currentness guards");
        if (options.observations.size() > 4) throw std::length_error("run: observation count exceeded");
        rewrite(TextHookPoint::input,control(),"prompt",prompt);
        std::vector<std::string> inputs; inputs.reserve(options.observations.size()); record->observation_inputs_.reserve(options.observations.size());
        for (const auto& observation : options.observations) {
            if (!observation) throw std::invalid_argument("run: observation required");
            inputs.push_back(observation_input(*observation,now)); const auto& data = observation->metadata();
            record->observation_inputs_.push_back({data.source,data.provider_instance,data.object,data.revision,data.trust});
        }
        record->source_ = options.origin; record->goal_revision_ = options.goal_revision;
        if (options.planning) {
            planning_scope.emplace(run,options.goal_revision,options.tools,*options.planning);
            planning_request = prepare_plan_prompt(*planning_scope,0,prompt);
            record->plan_ = std::make_unique<PlanBook>(*planning_scope);
            record->plan_caller_ = options.confirmation_caller;
            record->plan_outputs_.reserve(options.policy.model_limit);
            record->plan_revisions_.reserve(options.planning->maximum_revisions);
            record->goal_revisions_.reserve(options.goal_update_limit); record->previous_plans_.reserve(options.goal_update_limit);
            record->observation_rebinds_.reserve(options.observation_rebind_limit);
            record->plan_feedback_.reserve(64);
            planning_goal = prompt;
        }
        record->context_ = std::make_unique<RunContext>(std::move(history),std::move(prompt),std::move(inputs));
        record->versions_ = public_run_versions(config,options,deadline > now ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now) : std::chrono::milliseconds::zero());
        record->model_steps_.reserve(options.policy.model_limit); record->batches_.reserve(options.policy.model_limit);
        record->model_contexts_.reserve(options.policy.model_limit);
        if (options.confirmation_caller) record->confirmations_ = std::make_unique<ConfirmationBook>(run,*options.confirmation_caller,options.confirmation_limit,options.budget,&record->journal_);
        action_basis->observations = std::move(options.action_observations); options.action_observations.reset();
        // 最后检查只持有原操作读句柄与共享依据，避免捕获整个运行或把旧回执变成新的执行器。
        const auto action_guard = [clock = options.now,absolute = deadline,run_state = record->operation(),basis = action_basis] {
            const auto current = clock();
            if (run_state->state() != runtime::OperationState::pending || run_state->cancellation_reason() != runtime::CancelReason::none ||
                current >= absolute || (basis->observations && (!basis->available || !basis->observations->current(current)))) return false;
            // 开始尝试变更后不再复用同一行动前依据；后续只读查询仍可继续。
            if (basis->observations) basis->available = false;
            return true;
        };
        if (options.tools) record->tools_ = std::make_unique<ToolExecutor>(run,*options.tools,
            ToolLimits{options.policy.tool_limit,std::min(options.timeout,std::chrono::milliseconds(30000))},options.budget,options.now,action_guard);
        ++resident_count;
    }
    // 已退出或外层尚未启动的状态销毁后归还运行名额。
    ~RunLoopState() { --resident_count; }
    // 从原运行生成控制上下文，清理期间不再调用可能已失效的外部时钟。
    // 步骤：读取取消首因 → 区分活动与收尾 → 复制身份、版本和预算。
    PolicyContext control() const {
        auto reason = record->operation()->cancellation_reason();
        const bool stopping = record->operation()->state() != runtime::OperationState::pending;
        if (stopping && reason == runtime::CancelReason::none) reason = runtime::CancelReason::owner_stop;
        PolicyContext context{record->attempt_,options.goal_revision,options.policy.revision,options.origin,record->model_requests_,record->tool_requests_,
            stopping ? deadline : options.now(),deadline,reason};
        if (plan_step && !planning_request) { context.plan_revision = plan_step->key.plan.plan_revision; context.plan_step = plan_step->index+1; }
        if (action_basis->observations) {
            context.observation_binding = action_basis->observations->binding();
            context.observations_current = action_basis->available && action_basis->observations->current(context.now);
        }
        return context;
    }
    // 接纳当前字段的可选改写，所有 Agent 调用者共用相同的文本边界。
    // 步骤：调用明确的宿主转换 → 核对输出容量与编码 → 更新原字段。
    bool rewrite(TextHookPoint point, const PolicyContext& context, std::string_view field, std::string& value,
        std::size_t limit = 1024*1024) {
        if (!options.text_hook) return false;
        auto changed = options.text_hook(point,context,field,value,limit);
        if (!changed) return false;
        if (changed->size() > limit || !runtime::valid_utf8(*changed)) throw std::invalid_argument("run: invalid transformed field");
        value = std::move(*changed);
        return true;
    }
    // 使用改写后的模型文本和参数进入正常响应校验，保留模型调用身份和工具元数据。
    // 步骤：改写助手文本 → 逐个改写同名工具的原参数 → 复核完整响应。
    bool rewrite_response(model::Response& response) {
        const auto current = control();
        auto changed = rewrite(TextHookPoint::model_response,current,"content",response.message.content);
        for (auto& call : response.message.tool_calls)
            changed = rewrite(TextHookPoint::tool_arguments,current,call.name,call.arguments,64*1024) || changed;
        if (changed) model::validate_response(response);
        return changed;
    }
    // 在宿主线程发布可选的取证记录。采集失败不改变原运行裁定。
    void capture(ModelCapturePhase phase, AttemptId attempt, const model::Request* input = nullptr,
        const model::Response* output = nullptr, const model::CallDiagnostics* call = nullptr,
        runtime::OperationState result = runtime::OperationState::pending) noexcept {
        if (!options.model_capture) return;
        try { options.model_capture({phase,attempt,config.provider,config.model,input,output,call,result}); }
        catch (...) {}
    }
    // 停止新子调用，实际模型及工具退出仍由原完成源确认。
    void stop_children(runtime::CancelReason reason) {
        if (record->plan_) record->plan_->stop();
        if (plan_step && plan_step->result.operation()->state() == runtime::OperationState::pending)
            plan_step->result.settle(record->operation()->state() == runtime::OperationState::failed ? runtime::OperationState::failed : runtime::OperationState::cancelled);
        if (record->confirmations_) record->confirmations_->advance(deadline,reason);
        if (record->model_) record->model_->request_cancel(reason);
        if (record->tools_) record->tools_->close(reason);
    }
    // 固定唯一失败及原因，保留已提交上下文和实际动作效果。
    // 步骤：仅结算未终止业务 → 保存失败事实 → 请求子工作停止。
    void fail(std::exception_ptr error) {
        if (record->operation()->state() == runtime::OperationState::pending) {
            if (stage == TurnFailure::control && record->stop_reason_ == PolicyReason::none) record->stop_reason_ = PolicyReason::policy_failed;
            record->error_ = std::move(error); record->failure_ = stage; record->result_.settle(runtime::OperationState::failed);
            record->settled_ = std::chrono::steady_clock::now(); record->record_fact(RunFactKind::settled);
        }
        stop_children(runtime::CancelReason::owner_stop);
    }
    // 在各执行边界裁定用户取消或期限，不将逻辑取消当作资源退出。
    // 步骤：核对业务终态 → 检查期限及首个取消 → 请求子工作停止并记录业务取消。
    void interrupt() {
        if (record->operation()->state() != runtime::OperationState::pending) return;
        if (record->operation()->cancellation_reason() == runtime::CancelReason::none && options.now() >= deadline)
            record->request_cancel(runtime::CancelReason::deadline);
        const auto reason = record->operation()->cancellation_reason();
        if (reason == runtime::CancelReason::none) return;
        record->stop_reason_ = reason == runtime::CancelReason::deadline ? PolicyReason::deadline : PolicyReason::cancelled;
        stop_children(reason); record->result_.settle(runtime::OperationState::cancelled);
        record->settled_ = std::chrono::steady_clock::now(); record->record_fact(RunFactKind::settled);
    }
    // 处理宿主决定时重查原目标，历史回执不依赖工具仍在线，也不能重新执行。
    // 步骤：保护入口并复用原回执 → 裁定取消和期限 → 重查原授权绑定 → 保存单次决定。
    ConfirmationReceipt decide(const ConfirmationCaller& caller, const ConfirmationCommand& command) {
        if (advancing || !record->confirmations_) throw std::logic_error("run: reentrant or unavailable confirmation");
        advancing = true;
        try {
            auto& book = *record->confirmations_;
            if (const auto prior = book.prior_receipt(caller,command)) { advancing = false; return *prior; }
            interrupt(); const auto current = control(); book.advance(current.now,current.cancellation);
            if (policy->check_context(current).verdict != PolicyVerdict::allow) throw std::invalid_argument("run: confirmation stopped");
            const auto original = book.target(command.request);
            try {
                const auto& scope = record->plan_ ? step_tools : options.tools;
                if (!scope) throw std::invalid_argument("run: confirmation scope unavailable");
                const auto selected = scope->resolve(original.name,original.arguments);
                if (selected.description.changes_state && !current.observations_current)
                    throw std::invalid_argument("run: action observations changed before confirmation");
                if (confirmation_target(current,selected,{original.call_id,original.name,original.arguments},original.candidate) != original)
                    throw std::invalid_argument("run: confirmation target changed");
            } catch (...) { book.invalidate(command.request,ConfirmationState::invalidated); throw; }
            const auto receipt = book.decide(caller,command,current.now); advancing = false; return receipt;
        } catch (...) { advancing = false; throw; }
    }
    // 步骤验证与工具授权分开，历史确认可重取但不能恢复停止的运行。
    // 步骤：核对宿主身份和重入 → 复用已完成步骤 → 重查运行与当前步骤 → 记录原宿主证据。
    void confirm_plan_step(const ConfirmationCaller& caller, const PlanStepKey& key, std::string_view evidence) {
        if (advancing || !record->plan_ || !record->plan_caller_ || caller != *record->plan_caller_)
            throw std::invalid_argument("run: unbound or reentrant plan verification");
        for (const auto& previous : record->previous_plans_) if (previous->binding().goal_revision == key.plan.goal_revision) {
            previous->confirm(key,evidence); return;
        }
        advancing = true;
        try {
            const auto snapshot = record->plan_->snapshot();
            const bool completed = std::any_of(snapshot.steps.begin(),snapshot.steps.end(),[&](const auto& step) {
                return step.key == key && step.state == PlanStepState::completed;
            });
            if (!completed) {
                interrupt();
                if (policy->check_context(control()).verdict != PolicyVerdict::allow || !plan_step || plan_step->key != key)
                    throw std::invalid_argument("run: stale or stopped plan verification");
            }
            record->plan_->confirm(key,evidence); advancing = false;
        } catch (...) { advancing = false; throw; }
    }
    // 重规划仅替换尚未执行的尾部，不重置运行次数、期限或已发生事实。
    // 步骤：核对调用者并复用原请求 → 重查停止及步骤边界 → 准备受保护前缀的候选请求 → 发布待生成版本。
    PlanBinding replan(const ConfirmationCaller& caller, PlanBinding expected, std::string_view guidance) {
        if (advancing || !record->plan_ || !record->plan_caller_ || caller != *record->plan_caller_)
            throw std::invalid_argument("run: unbound or reentrant replan");
        for (const auto& previous : record->plan_revisions_) if (previous.expected == expected) {
            if (previous.guidance != guidance) throw std::invalid_argument("run: replan request conflict");
            return {expected.run,expected.goal_revision,expected.plan_revision+1};
        }
        advancing = true;
        try {
            interrupt(); const auto snapshot = record->plan_->snapshot();
            if (policy->check_context(control()).verdict != PolicyVerdict::allow || planning_request || record->model_ || batch ||
                expected != record->plan_->binding() || !snapshot.definition || snapshot.stopped || record->plan_revisions_.size() >= options.planning->maximum_revisions-1)
                throw std::invalid_argument("run: stale or unavailable replan");
            std::size_t prefix = 0;
            while (prefix < snapshot.steps.size() && (snapshot.steps[prefix].state == PlanStepState::completed || snapshot.steps[prefix].state == PlanStepState::skipped)) ++prefix;
            if (prefix < snapshot.steps.size() && snapshot.steps[prefix].state != PlanStepState::pending)
                throw std::logic_error("run: unfinished work or verification prevents replan");
            auto input = prepare_plan_prompt(*planning_scope,expected.plan_revision,planning_goal,snapshot.definition.get(),prefix,guidance);
            TurnRequest::PlanRevisionRecord receipt{expected,std::string(guidance)};
            record->plan_revisions_.push_back(std::move(receipt)); planning_request = std::move(input); advancing = false;
            return {expected.run,expected.goal_revision,expected.plan_revision+1};
        } catch (...) { advancing = false; throw; }
    }
    // 明确的新用户目标只在原工作退出边界生效，不通过新目标清空旧动作或重置运行预算。
    // 步骤：核对宿主并复用原修订 → 检查安全边界及总上限 → 准备新范围、策略和上下文 → 停止保留旧计划并发布新目标。
    PlanBinding revise_goal(const ConfirmationCaller& caller, PlanBinding expected, std::string_view goal) {
        if (advancing || !record->plan_ || !record->plan_caller_ || caller != *record->plan_caller_)
            throw std::invalid_argument("run: unbound or reentrant goal revision");
        for (const auto& previous : record->goal_revisions_) if (previous.expected == expected) {
            if (previous.goal != goal) throw std::invalid_argument("run: goal revision conflict");
            return {expected.run,expected.goal_revision+1,0};
        }
        advancing = true;
        try {
            interrupt();
            if (policy->check_context(control()).verdict != PolicyVerdict::allow || options.origin != RunOrigin::user_request ||
                expected != record->plan_->binding() || !expected.plan_revision || planning_request || record->model_ || batch ||
                (record->tools_ && record->tools_->busy()) || (plan_step && plan_step->exit.operation()->state() == runtime::OperationState::pending) ||
                record->goal_revisions_.size() == options.goal_update_limit || expected.goal_revision == std::numeric_limits<std::uint64_t>::max())
                throw std::invalid_argument("run: goal revision boundary unavailable");
            const auto revision = expected.goal_revision+1;
            PlanScope scope(record->run_id(),revision,options.tools,*options.planning);
            auto input = prepare_plan_prompt(scope,0,goal); auto book = std::make_unique<PlanBook>(scope);
            auto next_policy = std::make_unique<ExecutionPolicy>(record->run_id(),revision,options.policy);
            std::string next_goal(goal); TurnRequest::GoalRevisionRecord receipt{expected,next_goal};
            record->context_->revise_goal(revision,next_goal);

            record->plan_->stop(); record->previous_plans_.push_back(std::move(record->plan_));
            record->goal_revisions_.push_back(std::move(receipt)); record->plan_ = std::move(book);
            plan_step.reset(); step_tools.reset(); policy = std::move(next_policy); planning_scope = std::move(scope);
            planning_goal = std::move(next_goal); planning_request = std::move(input);
            options.goal_revision = revision; record->goal_revision_ = revision; advancing = false;
            return {expected.run,revision,0};
        } catch (...) { advancing = false; throw; }
    }
    // 反馈只属于当前未验证步骤，原工具退出可作为后续只读采样的起点。
    // 步骤：核对宿主和停止版本 → 检查当前步骤 → 核对整步退出或原工具关联及退出 → 返回同域时刻。
    ToolTime feedback_boundary(const ConfirmationCaller& caller, const PlanStepKey& key, runtime::DomainId completed_tool = 0) {
        if (!record->plan_ || !record->plan_caller_ || caller != *record->plan_caller_) throw std::invalid_argument("run: unbound feedback caller");
        interrupt(); const auto current = control();
        if (policy->check_context(current).verdict != PolicyVerdict::allow || !plan_step || plan_step->key != key || planning_request)
            throw std::invalid_argument("run: stale or stopped feedback");
        const auto snapshot = record->plan_->snapshot();
        if (std::none_of(snapshot.steps.begin(),snapshot.steps.end(),[&](const auto& step) {
            return step.key == key && step.state == PlanStepState::running && (completed_tool || step.waiting == PlanWait::verification);
        }))
            throw std::logic_error("run: original step has not reached verification");
        if (completed_tool) {
            const auto tools = record->tool_steps();
            const auto found = std::find_if(tools.begin(),tools.end(),[&](const auto& tool) { return tool.candidate == completed_tool; });
            if (found == tools.end() || !found->accepted || found->exit != runtime::OperationState::succeeded ||
                std::none_of(record->model_contexts_.begin(),record->model_contexts_.end(),[&](const auto& context) {
                    return context.attempt == found->attempt && context.goal_revision == key.plan.goal_revision && context.plan_revision == key.plan.plan_revision && context.plan_step == key.step;
                })) throw std::invalid_argument("run: feedback tool not exited in original plan step");
        }
        return current.now;
    }
    // 原工作退出后才固定新的反馈基线，模型旧观察或完成声明不能充当行动后证据。
    // 步骤：保护入口并检查原工作 → 核对当前基线及容量 → 完整准备条件和请求 → 登记有限签发次数。
    PlanFeedbackTicket request_plan_feedback(const ConfirmationCaller& caller, const PlanStepKey& key,
        ObservationGuard baseline, std::string field, runtime::Value expected, runtime::DomainId completed_tool) {
        if (advancing) throw std::logic_error("run: reentrant feedback request");
        advancing = true;
        try {
            const auto now = feedback_boundary(caller,key,completed_tool);
            if (!baseline.current(now) || record->feedback_requests_ >= 64) throw std::invalid_argument("run: invalid feedback baseline or capacity");
            const auto& metadata = baseline.observation()->metadata();
            PlanFeedbackSnapshot snapshot{0,key,record->attempt_,completed_tool,
                {metadata.source,metadata.provider_instance,metadata.object,metadata.revision,metadata.trust},expected.kind()};
            PlanFeedbackTicket ticket(key,std::move(baseline),std::move(field),expected,now,completed_tool);
            snapshot.request = ticket.id(); record->plan_feedback_.push_back(std::move(snapshot));
            ++record->feedback_requests_; advancing = false; return ticket;
        } catch (...) { advancing = false; throw; }
    }
    // 明确满足只记录原步骤的证据，不修改工具结果、未知效果或动作授权。
    // 步骤：保护入口并复核原步骤 → 检查新采样 → 满足时记录唯一证据 → 返回判定值。
    ObservationAssessment verify_plan_feedback(const ConfirmationCaller& caller, const PlanFeedbackTicket& ticket, const ObservationGuard& observation) {
        if (advancing) throw std::logic_error("run: reentrant feedback verification");
        advancing = true;
        try {
            const auto result = ticket.assess(observation,feedback_boundary(caller,ticket.step()));
            if (result.match == ObservationMatch::satisfied) record->plan_->confirm(ticket.step(),
                "feedback/"+std::to_string(ticket.id())+"/"+std::to_string(result.source)+"/"+std::to_string(result.revision));
            for (auto& snapshot : record->plan_feedback_) if (snapshot.request == ticket.id()) {
                snapshot.latest = result; snapshot.verified = result.match == ObservationMatch::satisfied;
                if (snapshot.checks != std::numeric_limits<std::uint64_t>::max()) ++snapshot.checks; else snapshot.checks_saturated = true;
                break;
            }
            advancing = false; return result;
        } catch (...) { advancing = false; throw; }
    }
    // 在原步骤待验证边界记录刷新水位，不恢复或替换尚未退出的工作。
    // 步骤：保护入口并核对原步骤 → 核对原绑定及签发上限 → 准备完整请求 → 登记签发次数。
    ObservationRebindTicket request_observation_rebind(const ConfirmationCaller& caller, const PlanStepKey& key) {
        if (advancing) throw std::logic_error("run: reentrant observation rebind request");
        advancing = true;
        try {
            const auto now = feedback_boundary(caller,key);
            if (!action_basis->observations || key.plan != record->plan_->binding() || record->model_ || batch ||
                (record->tools_ && record->tools_->busy()) || record->observation_rebind_requests_ >= options.observation_rebind_limit)
                throw std::invalid_argument("run: observation rebind boundary unavailable");
            ObservationRebindTicket ticket(key,*action_basis->observations,now);
            ++record->observation_rebind_requests_; advancing = false; return ticket;
        } catch (...) { advancing = false; throw; }
    }
    // 换入新输入及同一行动状态，不能借刷新重建工具执行器或清除未知效果。
    // 步骤：核对原步骤及绑定 → 校验完整新采样 → 预备全部输入与回执 → 重查停止 → 原子发布新依据。
    ObservationRebindReceipt rebind_observations(const ConfirmationCaller& caller, const ObservationRebindTicket& ticket, const ActionObservations& next_observations) {
        if (advancing || !ticket.binding() || ticket.step().plan.run != record->run_id() || !record->plan_caller_ || caller != *record->plan_caller_)
            throw std::invalid_argument("run: invalid or reentrant observation rebind");
        for (const auto& previous : record->observation_rebinds_) if (previous.request == ticket.id()) {
            if (previous.previous_binding != ticket.binding() || previous.step != ticket.step() || previous.binding != next_observations.binding())
                throw std::invalid_argument("run: observation replacement conflict");
            return previous;
        }
        advancing = true;
        try {
            const auto now = feedback_boundary(caller,ticket.step());
            if (ticket.step().plan != record->plan_->binding() || !action_basis->observations || ticket.binding() != action_basis->observations->binding() ||
                record->model_ || batch || (record->tools_ && record->tools_->busy()) || !ticket.accepts(next_observations,now))
                throw std::invalid_argument("run: stale or incomplete observation replacement");

            auto replacement = std::optional<ActionObservations>(next_observations); auto observations = replacement->observations();
            ObservationRebindReceipt receipt{ticket.id(),ticket.binding(),replacement->binding(),ticket.step(),{}};
            receipt.after_model_requests = record->model_requests_;
            std::vector<std::string> inputs; inputs.reserve(observations.size()); receipt.observations.reserve(observations.size());
            for (const auto& observation : observations) {
                inputs.push_back(observation_input(*observation,now)); const auto& data = observation->metadata();
                receipt.observations.push_back({data.source,data.provider_instance,data.object,data.revision,data.trust});
            }
            auto stored = receipt;
            interrupt();
            if (policy->check_context(control()).verdict != PolicyVerdict::allow || !replacement->current(options.now()))
                throw std::invalid_argument("run: observation replacement stopped before publication");
            record->context_->refresh_observations(replacement->binding(),std::move(inputs));

            record->observation_rebinds_.push_back(std::move(stored)); options.observations = std::move(observations);
            action_basis->observations = std::move(replacement); action_basis->available = true;
            advancing = false; return receipt;
        } catch (...) { advancing = false; throw; }
    }
    // 先运行模型前控制点，再提交不可变消息；提交后不再分配运行元数据。
    // 步骤：裁定停止 → 检查规则及观察资格 → 准备完整输入 → 重查停止后接纳并发布步骤身份。
    void start_model() {
        interrupt(); if (record->operation()->state() != runtime::OperationState::pending) return;
        stage = TurnFailure::control; auto current = control(); current.attempt.step.sequence = record->model_requests_+1;
        const auto decision = restrict_policy(policy->before_model(current),options.extension,ControlPoint::model_request,current);
        TraceFact policy_fact; policy_fact.subject = {record->run_id(),TraceNodeKind::attempt,current.attempt.step.sequence,current.attempt.sequence};
        policy_fact.kind = TraceFactKind::policy_decision; policy_fact.control_point = ControlPoint::model_request; policy_fact.decision = decision;
        record->journal_.append(policy_fact);
        interrupt(); if (record->operation()->state() != runtime::OperationState::pending) return;
        if (!record->model_requests_) for (const auto& observation : options.observations) {
            if (observation->validity(options.now()) != ObservationValidity::fresh || observation->revision_gap()) {
                record->stop_reason_ = PolicyReason::invalid_context; throw std::invalid_argument("run: observation invalidated before admission");
            }
        }
        if (decision.verdict != PolicyVerdict::allow) { record->stop_reason_ = decision.reason; throw std::runtime_error("run: model request denied"); }
        stage = TurnFailure::model_request;
        const auto& scope = record->plan_ ? step_tools : options.tools;
        model::Request input = planning_request ? planning_request->request :
            model::Request{record->context_->messages(),scope ? scope->definitions() : std::vector<model::ToolDefinition>{}};
        if (!options.system_instructions.empty()) {
            if (options.system_instructions.size() > 8192) throw std::length_error("run: oversized system instructions");
            if (input.messages.front().role == model::Role::system) input.messages.front().content =
                options.system_instructions + "\n" + input.messages.front().content;
            else input.messages.insert(input.messages.begin(), {model::Role::system, options.system_instructions});
        }
        const bool has_system = input.messages.front().role == model::Role::system;
        std::string instructions = has_system ? input.messages.front().content : std::string{};
        if (rewrite(TextHookPoint::model_request,current,"system",instructions,64*1024)) {
            if (has_system && instructions.empty()) input.messages.erase(input.messages.begin());
            else if (has_system) input.messages.front().content = std::move(instructions);
            else if (!instructions.empty()) input.messages.insert(input.messages.begin(),{model::Role::system,std::move(instructions)});
        }
        ModelContextSnapshot context{current.attempt,options.goal_revision};
        context.observation_binding = current.observation_binding;
        if (current.observation_binding) context.observations_current = current.observations_current;
        if (planning_request) { context.purpose = ModelPurpose::plan_generation; context.plan_revision = planning_request->expected.plan_revision+1; }
        else if (plan_step) { context.purpose = ModelPurpose::plan_step; context.plan_revision = plan_step->key.plan.plan_revision; context.plan_step = plan_step->key.step; }
        interrupt(); if (record->operation()->state() != runtime::OperationState::pending) return;
        capture(ModelCapturePhase::prepared,current.attempt,&input);
        auto request = models.submit_request(config,std::move(input),static_cast<bool>(options.model_capture));
        if (!record->model_requests_) record->origin_ = request->diagnostics().submitted;
        record->attempt_ = current.attempt; record->model_ = std::move(request); ++record->model_requests_;
        record->model_contexts_.push_back(std::move(context));
        TraceFact submitted; submitted.subject = {record->run_id(),TraceNodeKind::model,current.attempt.step.sequence,current.attempt.sequence,record->model_->diagnostics().request};
        submitted.kind = TraceFactKind::model_submitted; record->journal_.append(submitted);
        const auto call = record->model_->diagnostics();
        capture(ModelCapturePhase::submitted,current.attempt,nullptr,nullptr,&call);
    }
    // 将下一计划步骤交给原模型工具循环，工作成功仍须另行由宿主验证。
    // 步骤：取得下一未完成步骤 → 收紧原工具范围并预留工作 → 准备步骤输入与关联 → 提交原模型入口。
    void start_plan_step() {
        const auto snapshot = record->plan_->snapshot();
        if (!snapshot.definition || snapshot.stopped) return;
        std::size_t index = 0;
        while (index < snapshot.steps.size() && (snapshot.steps[index].state == PlanStepState::completed || snapshot.steps[index].state == PlanStepState::skipped)) ++index;
        if (index == snapshot.steps.size() || snapshot.steps[index].state != PlanStepState::pending) return;
        const auto& step = snapshot.definition->steps()[index];
        auto tools = options.tools ? std::optional<ToolSnapshot>(options.tools->restrict_to(step.tools)) : std::nullopt;
        auto execution = std::make_unique<PlanExecution>(snapshot.steps[index].key,index);
        record->context_->begin_step(plan_step_input(*snapshot.definition,index));
        const AttemptId attempt{{record->run_id(),record->model_requests_+1},1};
        record->plan_->start(execution->key,{attempt,execution->result.operation(),execution->exit.operation()});
        step_tools = std::move(tools); plan_step = std::move(execution); start_model();
    }
    // 只有宿主已经确认的步骤才能让出下一步骤，最终结算仍使用原 Run 完成源。
    // 步骤：读取已验证步骤 → 保留最后步骤文本并释放已退出工作 → 全部完成则结算，否则下次推进再调度。
    void advance_plan() {
        if (!plan_step) { start_plan_step(); return; }
        const auto snapshot = record->plan_->snapshot();
        const bool confirmed = std::any_of(snapshot.steps.begin(),snapshot.steps.end(),[&](const auto& step) {
            return step.key == plan_step->key && step.state == PlanStepState::completed;
        });
        if (!confirmed) return;
        record->answer_ = std::move(plan_step->answer); plan_step.reset(); step_tools.reset();
        if (!std::all_of(snapshot.steps.begin(),snapshot.steps.end(),[](const auto& step) {
            return step.state == PlanStepState::completed || step.state == PlanStepState::skipped;
        })) return;
        stage = TurnFailure::control;
        rewrite(TextHookPoint::reply,control(),"answer",record->answer_);
        interrupt(); if (record->operation()->state() != runtime::OperationState::pending) return;
        record->result_.settle(runtime::OperationState::succeeded);
        record->settled_ = std::chrono::steady_clock::now(); record->record_fact(RunFactKind::settled);
    }
    // 保存已退出模型的原状态和用量，释放请求槽后才允许下一次提交。
    // 步骤：复制真实状态及可选用量 → 保存步骤与首个退出事实 → 归还模型请求槽。
    void collect_model() {
        ModelStepSnapshot value{record->attempt_,record->model_->diagnostics(),record->model_->operation()->state(),record->model_->exit()->state()};
        if (value.result == runtime::OperationState::succeeded) value.usage = record->model_->response().usage;
        value.observed_exit = std::chrono::steady_clock::now();
        capture(ModelCapturePhase::exited,value.attempt,nullptr,record->model_->observed_response(),&value.call,value.result);
        record->model_steps_.push_back(value); record->completed_call_ = value.call;
        record->completed_model_result_ = value.result; record->completed_model_exit_ = value.exit;
        TraceFact fact; fact.subject = {record->run_id(),TraceNodeKind::model,value.attempt.step.sequence,value.attempt.sequence,value.call.request};
        fact.kind = TraceFactKind::model_exited; fact.result = value.result; fact.exit = value.exit; fact.model_outcome = value.call.outcome;
        record->journal_.append(fact);
        record->record_fact(RunFactKind::model_finished); record->model_.reset(); models.collect();
    }
    // 仅在全部真实工作退出后结算运行退出，不丢弃未完成工具占位。
    // 步骤：核对全部工作已退出 → 封闭上下文和模型接纳 → 发布运行退出。
    void finish() {
        if (record->operation()->state() == runtime::OperationState::pending || record->model_ || (record->tools_ && record->tools_->busy()) || (batch && !batch->done())) return;
        if (plan_step && plan_step->exit.operation()->state() == runtime::OperationState::pending) plan_step->exit.settle(runtime::OperationState::succeeded);
        record->context_->seal(); models.close(); record->exited_ = std::chrono::steady_clock::now();
        record->exit_.settle(runtime::OperationState::succeeded); record->record_fact(RunFactKind::exited);
        if (options.exit_capture) {
            try { options.exit_capture(*record); }
            catch (...) {}
        }
    }
    // 推进一个阶段，模型与工具之间让出宿主，所有失败仍继续收尾。
    // 步骤：裁定停止并接收模型 → 推进工具或处理模型响应 → 提交上下文 → 按限额继续或确认退出。
    void advance() {
        using namespace runtime;
        if (advancing) throw std::logic_error("run: reentrant advance");
        if (record->exit()->state() != OperationState::pending) return;
        advancing = true;
        try {
            stage = TurnFailure::control; interrupt(); models.advance();
            if (batch) {
                stage = TurnFailure::tool_execution;
                if (batch->advance(control())) {
                    record->record_history();
                    const auto continuation = batch->continuation(); const auto stamp = batch->progress_fingerprint(); batch = nullptr; interrupt();
                    if (record->operation()->state() == OperationState::pending) {
                        stage = TurnFailure::control;
                        if (continuation.verdict != PolicyVerdict::allow) { record->stop_reason_ = continuation.reason; throw std::runtime_error("run: continuation denied"); }
                        repeated = last_fingerprint && *last_fingerprint == stamp ? repeated+1 : 0; last_fingerprint = stamp;
                        if (repeated >= options.no_progress_limit) { record->stop_reason_ = PolicyReason::no_progress; throw std::runtime_error("run: no progress"); }
                        start_model();
                    }
                }
            } else if (record->model_ && record->model_->exit()->state() != OperationState::pending) {
                std::optional<model::Response> response; stage = TurnFailure::model;
                try { if (record->operation()->state() == OperationState::pending) response = record->model_->response(); }
                catch (...) { collect_model(); throw; }
                collect_model(); interrupt();
                if (response && record->operation()->state() == OperationState::pending) {
                    stage = TurnFailure::model_response;
                    if (rewrite_response(*response)) {
                        const auto& call = record->completed_call_;
                        capture(ModelCapturePhase::rewritten,record->attempt_,nullptr,&*response,&call,OperationState::succeeded);
                    }
                    interrupt();
                }
                if (response && record->operation()->state() == OperationState::pending) {
                    if (planning_request) {
                        stage = TurnFailure::model_response;
                        const auto candidate = parse_plan_response(*planning_scope,planning_request->expected,*response);
                        record->plan_->install(planning_request->expected,candidate.steps()); planning_request.reset();
                    } else if (response->finish == model::FinishReason::stop) {
                        if (options.observation_action && !record->tool_requests_) {
                            stage = TurnFailure::control; record->stop_reason_ = PolicyReason::invalid_result;
                            throw std::runtime_error("run: observed action has no tool receipt");
                        }
                        stage = TurnFailure::history_commit;
                        if (plan_step) {
                            PlanStepOutput output{plan_step->key,record->attempt_,response->message.content};
                            record->context_->commit_step_text(response->message.content); record->record_history();
                            record->plan_outputs_.push_back(std::move(output));
                            plan_step->answer = std::move(response->message.content);
                            plan_step->result.settle(OperationState::succeeded); plan_step->exit.settle(OperationState::succeeded);
                        } else {
                            auto answer = response->message.content;
                            stage = TurnFailure::control;
                            rewrite(TextHookPoint::reply,control(),"answer",answer);
                            interrupt();
                            if (record->operation()->state() == OperationState::pending) {
                                stage = TurnFailure::history_commit;
                                record->context_->finish_text(std::move(response->message.content)); record->record_history();
                                record->answer_ = std::move(answer); record->result_.settle(OperationState::succeeded);
                                record->settled_ = std::chrono::steady_clock::now(); record->record_fact(RunFactKind::settled);
                            }
                        }
                    } else if (response->finish == model::FinishReason::tool_calls && record->tools_) {
                        stage = TurnFailure::tool_execution; const auto count = response->message.tool_calls.size();
                        if (record->tool_requests_ > options.policy.tool_limit || count > options.policy.tool_limit-record->tool_requests_) {
                            stage = TurnFailure::control; record->stop_reason_ = PolicyReason::tool_limit; throw std::length_error("run: tool limit exhausted");
                        }
                        const auto& scope = record->plan_ ? step_tools : options.tools;
                        auto next_batch = std::make_unique<ToolBatch>(*record->context_,*record->tools_,response->message,*scope,*policy,control(),options.extension,record->confirmations_.get(),&record->journal_);
                        batch = next_batch.get(); record->batches_.push_back(std::move(next_batch)); record->tool_requests_ += count;
                    } else { stage = TurnFailure::model_response; throw std::runtime_error("run: unsupported model response"); }
                }
            } else if (record->operation()->state() == OperationState::pending && record->plan_ && !record->model_) {
                if (planning_request) start_model(); else advance_plan();
            }
            finish();
        } catch (...) { fail(std::current_exception()); try { finish(); } catch (...) {} }
        advancing = false;
    }
};
// 完整构造后提交首个模型请求，拒绝不发布运行对象。
// 步骤：核对运行容量 → 准备完整状态 → 提交模型 → 记录接纳事实。
RunLoop::RunLoop(RunId run, model::Config config, model::Backend backend, std::shared_ptr<const MessageHistory> history, std::string prompt, RunOptions options) {
    if (resident_count >= 64) throw std::length_error("run: resident capacity exhausted");
    state_ = std::make_unique<RunLoopState>(run,std::move(config),std::move(backend),std::move(history),std::move(prompt),std::move(options));
    state_->start_model();
    if (!state_->record->model_requests_) throw std::runtime_error("run: stopped before model admission");
    state_->record->accepted_ = std::chrono::steady_clock::now();
    if (!state_->record->model_requests_) state_->record->origin_ = state_->record->accepted_;
    state_->record->record_fact(RunFactKind::accepted);
}
// 关闭局部协调器后仍保留未退出模型和工具，不依赖客户端推进。
// 步骤：请求停止并尝试推进 → 未退出转入保留链，否则释放上下文。
RunLoop::~RunLoop() {
    state_->record->request_cancel(runtime::CancelReason::owner_stop);
    try { state_->advance(); } catch (...) {}
    if (state_->record->exit()->state() == runtime::OperationState::pending) {
        auto* value = state_.release(); value->next = retained_head; retained_head = value; ++retained_count;
    } else state_->record->context_.reset();
}
// 返回原回合持有，不复制完成权。
std::shared_ptr<TurnRequest> RunLoop::request() const noexcept { return state_->record; }
// 读取最新完整上下文，调用不会产生模型或工具工作。
std::shared_ptr<const MessageHistory> RunLoop::history() const noexcept { return state_->record->context_->committed(); }
// 推进当前运行的一个阶段。
void RunLoop::advance() { state_->advance(); }
// 只转交可信控制命令，不驱动模型或工具执行。
ConfirmationReceipt RunLoop::decide(const ConfirmationCaller& caller, const ConfirmationCommand& command) { return state_->decide(caller,command); }
// 转交可信验证，调用本身不继续运行或改变原工作结果。
void RunLoop::confirm_plan_step(const ConfirmationCaller& caller, const PlanStepKey& key, std::string_view evidence) { state_->confirm_plan_step(caller,key,evidence); }
// 转交有限重规划请求，不在命令提交期间执行模型。
PlanBinding RunLoop::replan(const ConfirmationCaller& caller, PlanBinding expected, std::string_view guidance) { return state_->replan(caller,expected,guidance); }
// 转交用户目标修订，原模型和工具仍由同一运行拥有。
PlanBinding RunLoop::revise_goal(const ConfirmationCaller& caller, PlanBinding expected, std::string_view goal) { return state_->revise_goal(caller,expected,goal); }
// 转交已退出步骤的反馈基线和宿主条件。
PlanFeedbackTicket RunLoop::request_plan_feedback(const ConfirmationCaller& caller, const PlanStepKey& key,
    ObservationGuard baseline, std::string field, runtime::Value expected, runtime::DomainId completed_tool) {
    return state_->request_plan_feedback(caller,key,std::move(baseline),std::move(field),expected,completed_tool);
}
// 只验证原请求的新观察，不在此调用中继续执行计划。
ObservationAssessment RunLoop::verify_plan_feedback(const ConfirmationCaller& caller, const PlanFeedbackTicket& ticket, const ObservationGuard& current) {
    return state_->verify_plan_feedback(caller,ticket,current);
}
// 由原运行在已退出步骤边界签发观察刷新请求。
ObservationRebindTicket RunLoop::request_observation_rebind(const ConfirmationCaller& caller, const PlanStepKey& key) { return state_->request_observation_rebind(caller,key); }
// 只更新原运行的输入与行动依据，不另建执行路径。
ObservationRebindReceipt RunLoop::rebind_observations(const ConfirmationCaller& caller, const ObservationRebindTicket& ticket, const ActionObservations& next) {
    return state_->rebind_observations(caller,ticket,next);
}
// 在关闭外层时等待原模型工作完成，不强制终止网络调用。
void RunLoop::wait_model() { state_->models.wait_closed(); }
// 返回保留运行数量。
std::size_t RunLoop::retained() noexcept { return retained_count; }
// 只重试已停止运行的收尾，处理过程中新增的保留项留到下次推进。
// 步骤：保护重试入口 → 分离当前保留链 → 推进并保留或回收各运行 → 归还入口。
std::size_t RunLoop::retry_retained() {
    if (reaping) throw std::logic_error("run: reentrant retained cleanup");
    reaping = true; auto* remaining = std::exchange(retained_head,nullptr);
    while (remaining) {
        auto* value = remaining; remaining = value->next;
        try { value->advance(); } catch (...) {}
        if (value->record->exit()->state() == runtime::OperationState::pending) { value->next = retained_head; retained_head = value; }
        else { value->record->context_.reset(); delete value; --retained_count; }
    }
    reaping = false; return retained_count;
}
}
