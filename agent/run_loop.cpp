#include <dafeiyu/run_loop.hpp>
#include <dafeiyu/session.hpp>
#include <algorithm>
#include <stdexcept>

namespace dfy::agent {
namespace {
RunLoopState* retained_head = nullptr;
std::size_t resident_count = 0, retained_count = 0;
bool reaping = false;
}
struct RunLoopState {
    std::shared_ptr<TurnRequest> record;
    model::Config config;
    model::AsyncClient models;
    RunOptions options;
    ExecutionPolicy policy;
    ToolTime deadline;
    ToolBatch* batch = nullptr;
    std::size_t repeated = 0;
    std::optional<std::uint64_t> last_fingerprint;
    TurnFailure stage = TurnFailure::model_request;
    RunLoopState* next = nullptr;
    bool advancing = false;
    // 在首个模型调用之前固定运行资源，全部分配完成后由外层启动。
    // 步骤：创建原回合与模型入口 → 校验选项 → 预留步骤及上下文 → 准备工具执行器。
    RunLoopState(RunId run, model::Config value, model::Backend backend, std::shared_ptr<const MessageHistory> history,
        std::string prompt, RunOptions settings)
        : record(new TurnRequest(run,settings.trace_capacity)), config(std::move(value)), models(1,std::move(backend)), options(std::move(settings)),
          policy(run,options.goal_revision,options.policy) {
        if (!options.now || options.timeout <= std::chrono::milliseconds::zero() || options.timeout > std::chrono::hours(1) ||
            !options.no_progress_limit || options.no_progress_limit > 64) throw std::invalid_argument("run: invalid limits");
        const auto now = options.now(); const auto duration = std::chrono::duration_cast<ToolTime::duration>(options.timeout);
        if (now > ToolTime::max()-duration) throw std::overflow_error("run: deadline overflow");
        deadline = now+duration; record->context_ = std::make_unique<RunContext>(std::move(history),std::move(prompt));
        record->versions_ = public_model_versions(config); auto& versions = record->versions_;
        versions.application_commit = options.version_labels.application_commit; versions.build = options.version_labels.build;
        versions.prompt_template = options.version_labels.prompt_template; versions.model_profile = options.version_labels.model_profile;
        versions.policy = options.policy.revision;
        if (options.tools) for (const auto& tool : options.tools->descriptions()) versions.tools.push_back({tool.id,tool.version,tool.instance,{},{}});
        validate_trace_versions(versions);
        record->model_steps_.reserve(options.policy.model_limit); record->batches_.reserve(options.policy.model_limit);
        if (options.confirmation_caller) record->confirmations_ = std::make_unique<ConfirmationBook>(run,*options.confirmation_caller,options.confirmation_limit,options.budget,&record->journal_);
        if (options.tools) record->tools_ = std::make_unique<ToolExecutor>(run,*options.tools,
            ToolLimits{options.policy.tool_limit,std::min(options.timeout,std::chrono::milliseconds(30000))},options.budget,options.now);
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
        return {record->attempt_,options.goal_revision,options.policy.revision,options.origin,record->model_requests_,record->tool_requests_,
            stopping ? deadline : options.now(),deadline,reason};
    }
    // 停止新子调用，实际模型及工具退出仍由原完成源确认。
    void stop_children(runtime::CancelReason reason) {
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
            if (policy.check_context(current).verdict != PolicyVerdict::allow) throw std::invalid_argument("run: confirmation stopped");
            const auto original = book.target(command.request);
            try {
                const auto selected = options.tools->resolve(original.name,original.arguments);
                if (confirmation_target(current,selected,{original.call_id,original.name,original.arguments},original.candidate) != original)
                    throw std::invalid_argument("run: confirmation target changed");
            } catch (...) { book.invalidate(command.request,ConfirmationState::invalidated); throw; }
            const auto receipt = book.decide(caller,command,current.now); advancing = false; return receipt;
        } catch (...) { advancing = false; throw; }
    }
    // 先运行模型前控制点，再提交不可变消息；提交后不再分配运行元数据。
    // 步骤：裁定停止 → 检查基础及扩展规则 → 准备完整输入 → 接纳并发布步骤身份。
    void start_model() {
        interrupt(); if (record->operation()->state() != runtime::OperationState::pending) return;
        stage = TurnFailure::control; auto current = control(); current.attempt.step.sequence = record->model_requests_+1;
        const auto decision = restrict_policy(policy.before_model(current),options.extension,ControlPoint::model_request,current);
        TraceFact policy_fact; policy_fact.subject = {record->run_id(),TraceNodeKind::attempt,current.attempt.step.sequence,current.attempt.sequence};
        policy_fact.kind = TraceFactKind::policy_decision; policy_fact.control_point = ControlPoint::model_request; policy_fact.decision = decision;
        record->journal_.append(policy_fact);
        interrupt(); if (record->operation()->state() != runtime::OperationState::pending) return;
        if (decision.verdict != PolicyVerdict::allow) { record->stop_reason_ = decision.reason; throw std::runtime_error("run: model request denied"); }
        stage = TurnFailure::model_request;
        model::Request input{record->context_->messages(),options.tools ? options.tools->definitions() : std::vector<model::ToolDefinition>{}};
        auto request = models.submit_request(config,std::move(input));
        if (!record->model_requests_) record->origin_ = request->diagnostics().submitted;
        record->attempt_ = current.attempt; record->model_ = std::move(request); ++record->model_requests_;
        TraceFact submitted; submitted.subject = {record->run_id(),TraceNodeKind::model,current.attempt.step.sequence,current.attempt.sequence,record->model_->diagnostics().request};
        submitted.kind = TraceFactKind::model_submitted; record->journal_.append(submitted);
    }
    // 保存已退出模型的原状态和用量，释放请求槽后才允许下一次提交。
    // 步骤：复制真实状态及可选用量 → 保存步骤与首个退出事实 → 归还模型请求槽。
    void collect_model() {
        ModelStepSnapshot value{record->attempt_,record->model_->diagnostics(),record->model_->operation()->state(),record->model_->exit()->state()};
        if (value.result == runtime::OperationState::succeeded) value.usage = record->model_->response().usage;
        value.observed_exit = std::chrono::steady_clock::now();
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
        record->context_->seal(); models.close(); record->exited_ = std::chrono::steady_clock::now();
        record->exit_.settle(runtime::OperationState::succeeded); record->record_fact(RunFactKind::exited);
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
                    if (response->finish == model::FinishReason::stop) {
                        stage = TurnFailure::history_commit; record->context_->finish_text(response->message.content);
                        record->record_history();
                        record->answer_ = std::move(response->message.content); record->result_.settle(OperationState::succeeded);
                        record->settled_ = std::chrono::steady_clock::now(); record->record_fact(RunFactKind::settled);
                    } else if (response->finish == model::FinishReason::tool_calls && record->tools_) {
                        stage = TurnFailure::tool_execution; const auto count = response->message.tool_calls.size();
                        if (record->tool_requests_ > options.policy.tool_limit || count > options.policy.tool_limit-record->tool_requests_) {
                            stage = TurnFailure::control; record->stop_reason_ = PolicyReason::tool_limit; throw std::length_error("run: tool limit exhausted");
                        }
                        auto next_batch = std::make_unique<ToolBatch>(*record->context_,*record->tools_,response->message,*options.tools,policy,control(),options.extension,record->confirmations_.get(),&record->journal_);
                        batch = next_batch.get(); record->batches_.push_back(std::move(next_batch)); record->tool_requests_ += count;
                    } else { stage = TurnFailure::model_response; throw std::runtime_error("run: unsupported model response"); }
                }
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
