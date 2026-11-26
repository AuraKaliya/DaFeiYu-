#include <dafeiyu/session.hpp>
#include <dafeiyu/run_loop.hpp>
#include <limits>

namespace dfy::agent {
using namespace runtime;
// 写入本批等待输入的实际送模步骤或未带入原因，修订在整批更新后发布。
void TurnRequest::settle_inputs(InputState state, std::uint64_t step) noexcept {
    if (settled_inputs_ == inputs_.size()) return;
    while (settled_inputs_ < inputs_.size()) {
        auto& input = inputs_[settled_inputs_++];
        input.state = state;
        input.model_step = step;
    }
    ++input_revision_;
}

// 将补充绑定到原活动回合，关闭、重入或过期身份不会产生新工作。
InputReceipt Session::append_input(RunId expected, std::string text, std::optional<std::int64_t> event_utc_ms) {
    if (working_ || !accepting() || !active_ || active_->request()->run_id() != expected) return {InputAdmission::closed};
    return active_->append_input(std::move(text),event_utc_ms);
}

namespace {
// 将原文本调用包装为明确的文本后端，不允许静默丢弃工具字段。
// 步骤：选取明确调用或内置文本 Provider → 包装完整响应并标记文本能力。
model::Backend text_backend(model::Completion completion) {
    if (!completion) completion = [](const auto& config,const auto& messages) { return model::complete(config,messages); };
    return {[completion = std::move(completion)](const model::Config& config,const model::Request& request) {
        return model::Response{{model::Role::assistant,completion(config,request.messages)}};
    },false};
}
// 定义跨插件共享的会话事实结构，不包含供应商参数或认证信息。
std::shared_ptr<const StructDescriptor> event_descriptor() {
    const FieldDescriptor fields[] = {{"kind",ValueKind::string,32},{"session",ValueKind::uint64,0},
        {"turn",ValueKind::uint64,0},{"messages",ValueKind::uint64,0}};
    return std::make_shared<const StructDescriptor>(StructDescriptor::create("dfy.session.event",1,fields,{4,64}));
}
}
// 保留完整回合结果的借用边界，失败和取消不能冒充空回答。
const std::string& TurnRequest::answer() const {
    if (operation()->state() == OperationState::failed) std::rethrow_exception(error_);
    if (operation()->state() == OperationState::cancelled) throw std::runtime_error("session: cancelled");
    if (operation()->state() != OperationState::succeeded) throw std::logic_error("session: answer pending");
    return answer_;
}
// 记录首次有效取消的时刻，重复请求或已结算回合不改写诊断。
// 步骤：交由原操作登记取消 → 仅在首次接纳时固定时间 → 返回原取消回执。
CancelDisposition TurnRequest::request_cancel(CancelReason reason) {
    const auto disposition = result_.request_cancel(reason);
    if (disposition == CancelDisposition::recorded) {
        cancel_requested_ = std::chrono::steady_clock::now(); record_fact(RunFactKind::cancel_requested);
    }
    return disposition;
}
// 用原操作状态和单次记录的时间形成回合视图，不维护第二套业务状态。
// 步骤：复制身份与时间 → 读取业务及退出状态 → 关联模型事实和通知接纳回执。
TurnDiagnostics TurnRequest::diagnostics() const noexcept {
    TurnDiagnostics result;
    result.session = run_id().session; result.turn = turn(); result.attempt = attempt_; result.accepted = accepted_; result.origin = origin_;
    result.cancel_requested = cancel_requested_; result.settled = settled_; result.exited = exited_;
    result.result = operation()->state(); result.exit = exit()->state();
    result.cancellation = operation()->cancellation_reason(); result.failure = failure_;
    result.model = model_ ? model_->diagnostics() : completed_call_;
    result.model_result = model_ ? model_->operation()->state() : completed_model_result_;
    result.model_exit = model_ ? model_->exit()->state() : completed_model_exit_;
    result.notifications = notifications_;
    result.model_requests = model_requests_; result.tool_requests = tool_requests_; result.stop_reason = stop_reason_;
    result.source = source_; result.goal_revision = goal_revision_;
    if (confirmations_) { result.confirmation_requests = confirmations_->size(); result.waiting_confirmations = confirmations_->waiting(); }
    return result;
}
// 在固定槽位记录首次发生的生命周期事实，重复推进不追加重复项。
// 步骤：定位本类事实槽 → 忽略已记录事实 → 复制原状态并签发宿主顺序修订。
void TurnRequest::record_fact(RunFactKind kind) noexcept {
    auto& fact = facts_[static_cast<std::size_t>(kind)];
    if (fact.revision) return;
    const auto current = diagnostics();
    fact = {++fact_count_,kind,std::chrono::steady_clock::now(),current.result,current.cancellation,
        current.model.outcome,current.failure,current.attempt,current.model.request};
    if (kind == RunFactKind::model_finished) return;
    TraceFact projection; projection.subject = {run_id()}; projection.result = current.result; projection.exit = current.exit;
    projection.cancellation = current.cancellation; projection.stop_reason = current.stop_reason;
    projection.kind = kind == RunFactKind::accepted ? TraceFactKind::accepted : kind == RunFactKind::cancel_requested ? TraceFactKind::cancel_requested :
        kind == RunFactKind::settled ? TraceFactKind::settled : TraceFactKind::exited;
    journal_.append(projection);
}
// 只在完整上下文已成为权威历史后投影一次提交，不把工具占位当成已提交事实。
void TurnRequest::record_history() noexcept {
    TraceFact fact; fact.subject = {run_id(),TraceNodeKind::attempt,attempt_.step.sequence,attempt_.sequence};
    fact.kind = TraceFactKind::history_committed; fact.source_revision = ++history_revision_; fact.history_messages = context_->committed()->size();
    journal_.append(fact);
}
// 复制诊断并按首次记录顺序排列事实，不触发后台结果接收或事件消费。
// 步骤：取得诊断值副本 → 按修订号复制已记录项 → 返回固定容量快照。
RunSnapshot TurnRequest::snapshot() const noexcept {
    RunSnapshot result; result.diagnostics = diagnostics(); result.fact_count = fact_count_;
    for (const auto& fact : facts_) if (fact.revision) result.facts[fact.revision-1] = fact;
    return result;
}
// 合并批次的只读值投影，查询不推进工作或延长代码寿命。
std::vector<BatchToolSnapshot> TurnRequest::tool_steps() const {
    std::vector<BatchToolSnapshot> result;
    for (const auto& batch : batches_) { auto values = batch->snapshot(); result.insert(result.end(),values.begin(),values.end()); }
    return result;
}
// 按宿主候选身份查询完整工具回执，值副本不持有原生代码或工作。
std::optional<ToolResult> TurnRequest::tool_result(runtime::DomainId candidate) const {
    for (const auto& batch : batches_) if (auto value = batch->result(candidate)) return value;
    return std::nullopt;
}
// 只复制确认值，不推进模型、工具或控制决定。
std::vector<ConfirmationSnapshot> TurnRequest::confirmations() const { return confirmations_ ? confirmations_->snapshot() : std::vector<ConfirmationSnapshot>{}; }
// 只复制普通追踪需要的确认元数据，不包含待审参数或命令。
std::vector<ConfirmationSummary> TurnRequest::confirmation_steps() const { return confirmations_ ? confirmations_->summaries() : std::vector<ConfirmationSummary>{}; }
// 只投影原计划记录，不把候选文本解释为执行或完成命令。
std::optional<PlanSnapshot> TurnRequest::plan() const { return plan_ ? std::optional<PlanSnapshot>(plan_->snapshot()) : std::nullopt; }
// 只返回同一原步骤已提交的模型文本，旧版本不会被当前同名步骤替代。
std::optional<PlanStepOutput> TurnRequest::plan_output(const PlanStepKey& key) const {
    for (const auto& output : plan_outputs_) if (output.key == key) return output;
    return std::nullopt;
}
// 复制已停止旧计划的值状态，旧目标不再有继续执行入口。
std::vector<PlanSnapshot> TurnRequest::plan_history() const {
    std::vector<PlanSnapshot> result; result.reserve(previous_plans_.size());
    for (const auto& previous : previous_plans_) result.push_back(previous->snapshot());
    return result;
}
// 完整准备会话数据和有界模型客户端，不创建待执行问答。
Session::Session(model::Config config, std::shared_ptr<events::BroadcastChannel> events,
    std::size_t result_capacity, model::Completion completion)
    : Session(std::move(config),std::move(events),text_backend(std::move(completion)),result_capacity) {}
// 完整准备结构化后端，状态仍由原会话的结果和退出操作负责。
Session::Session(model::Config config, std::shared_ptr<events::BroadcastChannel> events, model::Backend backend,
    std::size_t result_capacity, RunCaptureSinks capture)
    : config_(std::move(config)), events_(std::move(events)), descriptor_(event_descriptor()), id_(next_domain()),
      records_(result_capacity), backend_(std::move(backend)), capture_(std::move(capture)) {
    if (!events_ || !result_capacity || !backend_.complete) throw std::invalid_argument("session: channel, backend and result capacity required");
}
// 兜底关闭时先保留所有回合责任，实际模型退出后再结算回合退出。
// 步骤：关闭接纳 → 等待原模型退出 → 推进工具收尾 → 将未退出责任交给保留链。
Session::~Session() { close(); if (active_) active_->wait_model(); advance(); active_.reset(); }
// 以不可变事件传递会话事实，观察接纳或分配失败只形成独立回执。
// 步骤：构造类型化字段 → 创建宿主载荷 → 接纳观察通知或返回失败回执。
events::ObservationReceipt Session::notify(std::string_view kind, std::uint64_t turn) noexcept {
    try {
        const NamedValue values[] = {{"kind",Value::from_string(kind,32)},{"session",Value::from_uint64(id_)},
            {"turn",Value::from_uint64(turn)},{"messages",Value::from_uint64(history_->size())}};
        return events::observe(*events_,std::make_shared<const StructValue>(StructValue::create(descriptor_,values,64)));
    } catch (...) { return {events::ObservationStatus::failed,{}}; }
}
// 原入口沿用无工具选项，仍通过同一正式运行协调器执行。
std::shared_ptr<TurnRequest> Session::begin(const std::string& prompt) { return begin(prompt,{}); }
// 完整准备和控制检查成功后才接纳，拒绝不消耗回合号或改变已提交上下文。
// 步骤：检查入口与输入 → 包装关闭边界 → 准备有界协调器 → 固定接纳并通知开始。
std::shared_ptr<TurnRequest> Session::begin(const std::string& prompt, RunOptions options) {
    if (!*accepting_ || active_ || working_) throw std::logic_error("session: busy, closed or reentrant");
    collect();
    if (prompt.empty() || prompt.find_first_not_of(" \t\r\n") == std::string::npos || prompt.size() > 1024*1024)
        throw std::invalid_argument("session: nonblank prompt within 1 MiB required");
    if (turn_ == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("session: turn exhausted");
    if (!options.model_capture) options.model_capture = capture_.model;
    if (!options.exit_capture) options.exit_capture = capture_.exit;
    if (!options.tool_capture) options.tool_capture = capture_.tool;
    options.compare_projection = options.compare_projection || capture_.compare_projection;
    if (!options.extension && !options.text_hook && !options.version_labels.control_extension) options.version_labels.control_extension = "none";
    // 用独立会话门保护规则回调，关闭发生在回调内时仍能阻止新的受控调用。
    // 步骤：调用明确规则 → 重查会话门 → 返回原决定或关闭拒绝。
    options.extension = [gate = accepting_,rule = std::move(options.extension)](ControlPoint point,const PolicyContext& context) {
        const auto decision = rule ? rule(point,context) : PolicyDecision{PolicyVerdict::allow,PolicyReason::none};
        return *gate ? decision : PolicyDecision{PolicyVerdict::deny,PolicyReason::cancelled};
    };
    if (options.text_hook) {
        // 会话在原生回调中关停时，结束该字段的处理且不派发新工作。
        options.text_hook = [gate = accepting_,hook = std::move(options.text_hook)](auto point,const auto& context,auto field,auto value,auto limit) {
            auto changed = hook(point,context,field,value,limit);
            if (!*gate) throw std::logic_error("session: closed during text hook");
            return changed;
        };
    }
    working_ = true;
    try {
        for (auto& slot : records_) if (!slot) {
            const bool replace = options.replace_history;
            auto base = replace ? std::make_shared<const MessageHistory>() : history_;
            const auto epoch = replace ? next_domain() : evidence_epoch_;
            options.evidence_epoch = epoch;
            options.previous_status = replace ? nullptr : status_;
            auto next_status = options.context_status;
            auto status_binding = options.decision_status ? options.decision_status->binding : std::string{};
            if (options.decision_status && (status_binding.empty() || status_binding.size()>512))
                throw std::invalid_argument("session: invalid decision status binding");
            const bool reuse = decision_store_ && status_binding==status_binding_;
            auto store = !options.decision_status ? nullptr : reuse ? decision_store_ : std::make_shared<DecisionStatusStore>();
            const auto status_epoch = !store ? 0 : reuse ? status_epoch_ : next_domain();
            options.status_store = store;
            options.status_epoch = status_epoch;
            if (options.decision_status && !backend_.structured) options.decision_status->allow_read = false;
            auto loop = std::make_unique<RunLoop>(RunId{id_,turn_+1},config_,backend_,base,prompt,std::move(options));
            history_ = std::move(base);
            evidence_epoch_ = epoch;
            if (replace) status_.reset();
            active_status_ = std::move(next_status);
            decision_store_ = std::move(store);
            status_epoch_ = status_epoch;
            status_binding_ = std::move(status_binding);
            auto record = loop->request(); active_ = std::move(loop); slot = record; ++turn_;
            record->notifications_.started = notify("turn_started",turn_); working_ = false; return record;
        }
    } catch (...) { working_ = false; throw; }
    working_ = false; throw std::length_error("session: retained turn capacity exhausted");
}
// 路由可信控制命令，已退出结果只复用原回执，不重建执行器或恢复终态。
// 步骤：保护控制入口 → 按完整运行身份定位 → 活动运行重查或终态回执复用 → 归还入口。
ConfirmationReceipt Session::decide(const ConfirmationCaller& caller, const ConfirmationCommand& command) {
    if (working_) throw std::logic_error("session: reentrant decision");
    working_ = true;
    try {
        for (const auto& record : records_) if (record && record->run_id() == command.expected.attempt.step.run && record->confirmations_) {
            const auto receipt = active_ && active_->request() == record ? active_->decide(caller,command) :
                record->confirmations_->decide(caller,command,ToolTime::max());
            working_ = false; return receipt;
        }
    } catch (...) { working_ = false; throw; }
    working_ = false; throw std::invalid_argument("session: confirmation run unavailable");
}
// 验证原计划步骤，终态运行只允许重取已经保存的同一确认。
// 步骤：保护控制入口 → 按完整运行及宿主身份定位 → 活动运行复核或原证据幂等核对 → 归还入口。
void Session::confirm_plan_step(const ConfirmationCaller& caller, const PlanStepKey& key, std::string_view evidence) {
    if (working_) throw std::logic_error("session: reentrant plan verification");
    working_ = true;
    try {
        for (const auto& record : records_) if (record && record->run_id() == key.plan.run && record->plan_) {
            if (!record->plan_caller_ || caller != *record->plan_caller_) throw std::invalid_argument("session: unbound plan verifier");
            for (const auto& previous : record->previous_plans_) if (previous->binding().goal_revision == key.plan.goal_revision) {
                previous->confirm(key,evidence); working_ = false; return;
            }
            if (active_ && active_->request() == record) active_->confirm_plan_step(caller,key,evidence);
            else record->plan_->confirm(key,evidence);
            working_ = false; return;
        }
    } catch (...) { working_ = false; throw; }
    working_ = false; throw std::invalid_argument("session: plan run unavailable");
}
// 将重规划路由到原活动运行，终态只能复用已有请求的接纳结果。
// 步骤：保护入口并核对完整身份 → 复用同参数原请求 → 活动运行检查并接纳 → 归还入口。
PlanBinding Session::replan(const ConfirmationCaller& caller, PlanBinding expected, std::string_view guidance) {
    if (working_) throw std::logic_error("session: reentrant replan");
    working_ = true;
    try {
        for (const auto& record : records_) if (record && record->run_id() == expected.run && record->plan_) {
            if (!record->plan_caller_ || caller != *record->plan_caller_) throw std::invalid_argument("session: unbound replan caller");
            for (const auto& previous : record->plan_revisions_) if (previous.expected == expected) {
                if (previous.guidance != guidance) throw std::invalid_argument("session: replan request conflict");
                working_ = false; return {expected.run,expected.goal_revision,expected.plan_revision+1};
            }
            if (!active_ || active_->request() != record) throw std::logic_error("session: replan run already stopped");
            const auto receipt = active_->replan(caller,expected,guidance); working_ = false; return receipt;
        }
    } catch (...) { working_ = false; throw; }
    working_ = false; throw std::invalid_argument("session: replan run unavailable");
}
// 路由原运行的目标修订，重复旧请求只返回原接纳结果，不再次改变当前目标。
// 步骤：保护入口并核对调用者 → 复用完整原请求 → 活动运行检查及接纳 → 归还入口。
PlanBinding Session::revise_goal(const ConfirmationCaller& caller, PlanBinding expected, std::string_view goal) {
    if (working_) throw std::logic_error("session: reentrant goal revision");
    working_ = true;
    try {
        for (const auto& record : records_) if (record && record->run_id() == expected.run && record->plan_) {
            if (!record->plan_caller_ || caller != *record->plan_caller_) throw std::invalid_argument("session: unbound goal caller");
            for (const auto& previous : record->goal_revisions_) if (previous.expected == expected) {
                if (previous.goal != goal) throw std::invalid_argument("session: goal revision conflict");
                working_ = false; return {expected.run,expected.goal_revision+1,0};
            }
            if (!active_ || active_->request() != record) throw std::logic_error("session: goal run already stopped");
            const auto receipt = active_->revise_goal(caller,expected,goal); working_ = false; return receipt;
        }
    } catch (...) { working_ = false; throw; }
    working_ = false; throw std::invalid_argument("session: goal run unavailable");
}
// 只向当前活动运行签发反馈请求，旧回合不能借当前观察恢复执行。
// 步骤：核对活动身份与重入 → 原运行检查并签发 → 归还入口并返回不可变请求。
PlanFeedbackTicket Session::request_plan_feedback(const ConfirmationCaller& caller, const PlanStepKey& key,
    ObservationGuard baseline, std::string field, runtime::Value expected, runtime::DomainId completed_tool) {
    if (working_ || !active_ || active_->request()->run_id() != key.plan.run) throw std::invalid_argument("session: unavailable feedback run");
    working_ = true;
    try {
        auto ticket = active_->request_plan_feedback(caller,key,std::move(baseline),std::move(field),expected,completed_tool);
        working_ = false; return ticket;
    } catch (...) { working_ = false; throw; }
}
// 将反馈仅送回原活动运行，明确未知或未满足不变成动作重试。
// 步骤：核对原运行与重入 → 检查新观察并按需验证步骤 → 归还入口。
ObservationAssessment Session::verify_plan_feedback(const ConfirmationCaller& caller, const PlanFeedbackTicket& ticket, const ObservationGuard& current) {
    if (working_ || !active_ || active_->request()->run_id() != ticket.step().plan.run) throw std::invalid_argument("session: unavailable feedback run");
    working_ = true;
    try { const auto result = active_->verify_plan_feedback(caller,ticket,current); working_ = false; return result; }
    catch (...) { working_ = false; throw; }
}
// 只向原活动运行签发观察刷新，不让新回合继承旧回合的采样请求。
// 步骤：核对活动身份及重入 → 由原运行准备请求 → 归还入口并返回原请求。
ObservationRebindTicket Session::request_observation_rebind(const ConfirmationCaller& caller, const PlanStepKey& key) {
    if (working_ || !active_ || active_->request()->run_id() != key.plan.run) throw std::invalid_argument("session: unavailable observation rebind run");
    working_ = true;
    try { auto ticket = active_->request_observation_rebind(caller,key); working_ = false; return ticket; }
    catch (...) { working_ = false; throw; }
}
// 原请求重试只读取原回执，不重新启用已消费或停止的观察依据。
// 步骤：保护入口并核对宿主身份 → 复用或拒绝原请求 → 交给原活动运行更新 → 归还入口。
ObservationRebindReceipt Session::rebind_observations(const ConfirmationCaller& caller, const ObservationRebindTicket& ticket, const ActionObservations& next) {
    if (working_ || !ticket.binding()) throw std::invalid_argument("session: invalid or reentrant observation rebind");
    working_ = true;
    try {
        for (const auto& record : records_) if (record && record->run_id() == ticket.step().plan.run) {
            if (!record->plan_caller_ || caller != *record->plan_caller_) throw std::invalid_argument("session: unbound observation caller");
            for (const auto& previous : record->observation_rebinds_) if (previous.request == ticket.id()) {
                if (previous.previous_binding != ticket.binding() || previous.step != ticket.step() || previous.binding != next.binding())
                    throw std::invalid_argument("session: observation replacement conflict");
                auto receipt = previous; working_ = false; return receipt;
            }
            if (!active_ || active_->request() != record) throw std::invalid_argument("session: observation run already stopped");
            auto receipt = active_->rebind_observations(caller,ticket,next); working_ = false; return receipt;
        }
    } catch (...) { working_ = false; throw; }
    working_ = false; throw std::invalid_argument("session: observation run unavailable");
}
// 推进统一运行并同步完整历史，业务终态通知和真实退出分别处理。
// 步骤：保护宿主入口 → 推进一个运行阶段 → 采纳已提交上下文 → 发布唯一终态通知 → 退出后归还活动槽。
void Session::advance() {
    if (working_) throw std::logic_error("session: reentrant advance");
    if (!active_) return;
    working_ = true;
    try {
        active_->advance();
        const auto committed = active_->history();
        if (active_status_ && committed != history_) status_ = active_status_;
        history_ = committed;
        const auto record = active_->request();
        const auto state = record->operation()->state();
        if (state != OperationState::pending && !record->notifications_.finished)
            record->notifications_.finished = notify(state == OperationState::succeeded ? "turn_completed" :
                state == OperationState::cancelled ? "turn_cancelled" : "turn_failed",record->turn());
        if (record->exit()->state() != OperationState::pending) active_.reset();
    } catch (...) { working_ = false; throw; }
    working_ = false;
}
// 清空只允许在实际空闲时执行，不改变回合序号，也不调用模型。
events::ObservationReceipt Session::clear() {
    if (!*accepting_ || active_ || working_) throw std::logic_error("session: busy or closed");
    const auto epoch = next_domain();
    history_ = std::make_shared<const MessageHistory>();
    evidence_epoch_ = epoch;
    decision_store_.reset(); status_binding_.clear(); status_epoch_ = 0;
    status_.reset();
    active_status_.reset();
    return notify("session_cleared",0);
}
// 读取指定快照时重核有效会话与绑定代次，关闭的会话不再授权读取。
std::string Session::read_status(std::uint64_t epoch, std::string_view snapshot) const {
    if (!*accepting_ || !decision_store_ || epoch!=status_epoch_) return R"({"state":"unavailable","reason":"scope_mismatch"})";
    return decision_store_->read(id_,epoch,snapshot);
}
// 从本会话当前代次读取有限的原历史片段，过期代次、越界和未提交占位均不可见。
MessageHistory Session::read_evidence(std::uint64_t epoch, std::size_t first, std::size_t count) const {
    if (epoch != evidence_epoch_ || !first || !count || count > 65 || first-1 > history_->size() || count > history_->size()-(first-1))
        throw std::invalid_argument("session: invalid evidence reference");
    const auto begin = history_->begin()+(first-1);
    return {begin,begin+count};
}
// 停止新运行并请求当前工作退出，回调内关闭只登记取消而不重入推进。
// 步骤：关闭接纳 → 登记当前取消 → 非重入时推进收尾。
void Session::close() {
    *accepting_ = false; if (active_) active_->request()->request_cancel(CancelReason::owner_stop);
    if (!working_) advance();
}
// 返回原协调器保留的真实收尾数量。
std::size_t Session::retained() noexcept { return RunLoop::retained(); }
// 复用同一协调器收尾，不创建第二条执行路径。
std::size_t Session::retry_retained() { return RunLoop::retry_retained(); }
// 释放没有客户端的终态结果，尚未实际退出的回合始终保留。
void Session::collect() noexcept {
    for (auto& record : records_) if (record && record->exit()->state() != OperationState::pending && record.use_count() == 1) record.reset();
}
}
