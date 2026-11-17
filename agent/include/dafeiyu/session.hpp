#pragma once
#include <dafeiyu/async_model.hpp>
#include <dafeiyu/hooks.hpp>
#include <dafeiyu/run_identity.hpp>
#include <dafeiyu/run_context.hpp>
#include <dafeiyu/tool_batch.hpp>
#include <dafeiyu/run_options.hpp>
#include <dafeiyu/run_journal.hpp>
#include <dafeiyu/plan_book.hpp>
#include <array>

namespace dfy::agent {
class Session;
class RunLoop;
struct RunLoopState;
struct TurnNotifications { std::optional<events::ObservationReceipt> started, finished; };
enum class TurnFailure { none, model, history_commit, model_response, control, tool_execution, model_request };
// 固定大小的回合快照；状态来自原操作，接纳时刻可以晚于后台调用开始。
struct TurnDiagnostics {
    using TimePoint = model::CallDiagnostics::TimePoint;
    runtime::DomainId session = 0;
    std::uint64_t turn = 0;
    TimePoint accepted;
    TimePoint origin;
    std::optional<TimePoint> cancel_requested, settled, exited;
    runtime::OperationState result = runtime::OperationState::pending, exit = runtime::OperationState::pending;
    runtime::CancelReason cancellation = runtime::CancelReason::none;
    TurnFailure failure = TurnFailure::none;
    model::CallDiagnostics model;
    runtime::OperationState model_result = runtime::OperationState::pending, model_exit = runtime::OperationState::pending;
    TurnNotifications notifications;
    AttemptId attempt;
    std::size_t model_requests = 0, tool_requests = 0;
    std::size_t confirmation_requests = 0, waiting_confirmations = 0;
    PolicyReason stop_reason = PolicyReason::none;
    RunOrigin source = RunOrigin::user_request;
    std::uint64_t goal_revision = 0;
};
enum class RunFactKind { accepted, cancel_requested, model_finished, settled, exited };
// 已发生的生命周期事实；修订号反映宿主观察顺序，后台时间不必与修订顺序一致。
struct RunFact {
    std::uint64_t revision = 0;
    RunFactKind kind = RunFactKind::accepted;
    TurnDiagnostics::TimePoint observed;
    runtime::OperationState result = runtime::OperationState::pending;
    runtime::CancelReason cancellation = runtime::CancelReason::none;
    model::CallOutcome model = model::CallOutcome::pending;
    TurnFailure failure = TurnFailure::none;
    AttemptId attempt;
    runtime::DomainId model_request = 0;
};
struct ModelStepSnapshot {
    AttemptId attempt;
    model::CallDiagnostics call;
    runtime::OperationState result = runtime::OperationState::pending, exit = runtime::OperationState::pending;
    std::optional<model::Usage> usage;
    TurnDiagnostics::TimePoint observed_exit;
};
// 固定运行生命周期快照；model_finished 保留首个模型退出，全部步骤另由专用查询返回。
struct RunSnapshot {
    TurnDiagnostics diagnostics;
    std::array<RunFact,5> facts{};
    std::size_t fact_count = 0;
};
// 宿主线程内的一轮问答结果，业务终态、通知回执与后台退出分别保留。
class TurnRequest {
  public:
    // 禁止复制问答身份和完成权。
    TurnRequest(const TurnRequest&) = delete;
    // 禁止赋值或隐式移动已接受回合。
    TurnRequest& operator=(const TurnRequest&) = delete;
    // 返回本会话接受时固定的回合号。
    std::uint64_t turn() const noexcept { return run_id().sequence; }
    // 返回本轮接纳时固定的运行身份，与旧回合号共用一个来源。
    RunId run_id() const noexcept { return attempt_.step.run; }
    // 返回当前单次问答所属的第一步骤，不签发新身份或推进执行。
    StepId step_id() const noexcept { return attempt_.step; }
    // 返回当前单次模型尝试的身份，取消与实际退出不改变此值。
    AttemptId attempt_id() const noexcept { return attempt_; }
    // 返回问答业务状态，Succeeded 表示完整问答已提交历史。
    std::shared_ptr<const runtime::Operation> operation() const noexcept { return result_.operation(); }
    // 返回本轮后台资源已退出确认，取消业务不提前完成此操作。
    std::shared_ptr<const runtime::Operation> exit() const noexcept { return exit_.operation(); }
    // 请求取消当前回合，提交前由宿主推进裁定；已提交成功不回滚。
    runtime::CancelDisposition request_cancel(runtime::CancelReason reason);
    // 借用成功回答，失败重抛原错误；取消和尚未完成明确拒绝。
    const std::string& answer() const;
    // 返回观察接纳回执副本，观察失败不会改写业务结果。
    TurnNotifications notifications() const noexcept { return notifications_; }
    // 返回当前或已退出回合的诊断值副本，不保留模型请求、事件载荷或正文。
    TurnDiagnostics diagnostics() const noexcept;
    // 复制当前诊断与已发生事实，读取不推进执行且不依赖通知是否接纳。
    RunSnapshot snapshot() const noexcept;
    // 复制已实际退出的模型步骤，不返回后台请求或业务正文。
    std::vector<ModelStepSnapshot> model_steps() const { return model_steps_; }
    // 复制各批次工具的状态与策略决定，不持有原生代码。
    std::vector<BatchToolSnapshot> tool_steps() const;
    // 返回可信控制界面的确认记录和待审参数；普通诊断与通知不包含这些正文。
    std::vector<ConfirmationSnapshot> confirmations() const;
    // 返回无正文的有界增量；查询不推进运行，旧游标和投影故障由结果明确说明。
    TraceDelta trace_delta(std::uint64_t after = 0, std::size_t limit = 256) const { return journal_.read(after,limit); }
    // 复制确认元数据，普通追踪不通过完整确认接口读取业务参数。
    std::vector<ConfirmationSummary> confirmation_steps() const;
    // 返回运行接纳前固定的公开版本，缺失标签保持未知。
    TraceVersions trace_versions() const { return versions_; }
    // 复制接纳时的来源、对象及修订关联，不保留观察载荷或提供实例。
    std::vector<ObservationInputFact> observation_inputs() const { return observation_inputs_; }
    // 复制显式规划的候选及进度；无规划返回空值，查询不执行模型或推进步骤。
    std::optional<PlanSnapshot> plan() const;
  private:
    friend class Session;
    friend class RunLoop;
    friend struct RunLoopState;
    // 为尚未发布的回合固定第一步骤与第一次尝试，不调用模型或观察者。
    explicit TurnRequest(RunId run, std::size_t trace_capacity) : attempt_{{run,1},1},journal_(run,trace_capacity) {}
    AttemptId attempt_;
    RunJournal journal_;
    TraceVersions versions_;
    std::vector<ObservationInputFact> observation_inputs_;
    RunOrigin source_ = RunOrigin::user_request;
    std::uint64_t goal_revision_ = 0;
    // 在完整上下文提交后记录消息数量及提交修订，不复制业务正文。
    void record_history() noexcept;
    std::uint64_t history_revision_ = 0;
    // 首次写入指定事实并签发修订号，固定槽位预留确保收尾无需分配。
    void record_fact(RunFactKind kind) noexcept;
    std::array<RunFact,5> facts_{};
    std::size_t fact_count_ = 0;
    runtime::OperationSource result_{true}, exit_;
    std::string answer_;
    std::exception_ptr error_;
    TurnNotifications notifications_;
    std::shared_ptr<model::ModelRequest> model_;
    std::unique_ptr<RunContext> context_;
    std::unique_ptr<ToolExecutor> tools_;
    std::unique_ptr<ConfirmationBook> confirmations_;
    std::unique_ptr<PlanBook> plan_;
    std::optional<ConfirmationCaller> plan_caller_;
    std::vector<std::unique_ptr<ToolBatch>> batches_;
    std::vector<ModelStepSnapshot> model_steps_;
    std::size_t model_requests_ = 0, tool_requests_ = 0;
    PolicyReason stop_reason_ = PolicyReason::none;
    model::CallDiagnostics completed_call_;
    runtime::OperationState completed_model_result_ = runtime::OperationState::pending, completed_model_exit_ = runtime::OperationState::pending;
    TurnDiagnostics::TimePoint accepted_;
    TurnDiagnostics::TimePoint origin_;
    std::optional<TurnDiagnostics::TimePoint> cancel_requested_, settled_, exited_;
    TurnFailure failure_ = TurnFailure::none;
};

// 单活跃运行的异步会话，工具和规划均由宿主显式开启；不包含磁盘持久化。
class Session {
  public:
    // 固定结果槽、共享事件通道与模型调用，历史正文总量最多 1 MiB。
    Session(model::Config config, std::shared_ptr<events::BroadcastChannel> events,
        std::size_t result_capacity = 4, model::Completion completion = {});
    // 使用显式结构化模型后端，复用同一会话、历史和结果生命周期。
    Session(model::Config config, std::shared_ptr<events::BroadcastChannel> events,
        model::Backend backend, std::size_t result_capacity = 4);
    // 取消并等待仍在使用会话输入的模型调用退出，外部回合结果继续有效。
    ~Session();
    // 禁止复制会话身份、历史及后台责任。
    Session(const Session&) = delete;
    // 禁止赋值或隐式移动进行中的会话。
    Session& operator=(const Session&) = delete;
    // 空闲时接受一轮问答，完整准备和模型提交成功后才发布开始通知。
    std::shared_ptr<TurnRequest> begin(const std::string& prompt);
    // 用明确的工具范围、策略和限额接纳一次运行，旧入口保持无工具行为。
    std::shared_ptr<TurnRequest> begin(const std::string& prompt, RunOptions options);
    // 使用宿主绑定凭证处理确认命令，不在提交决定时执行动作；保留结果期间可重取原回执。
    ConfirmationReceipt decide(const ConfirmationCaller& caller, const ConfirmationCommand& command);
    // 由宿主绑定的验证入口确认步骤完成条件；重复同证据不重复推进，调用本身不执行后续工作。
    void confirm_plan_step(const ConfirmationCaller& caller, const PlanStepKey& key, std::string_view evidence);
    // 推进一个有界阶段并采纳完整上下文，业务终态后仍等待实际退出；通知仅入队。
    void advance();
    // 空闲时清空已提交历史并通知，保持会话身份及回合号。
    events::ObservationReceipt clear();
    // 停止新回合并请求取消，等待实际退出期间仍可推进事件和管理操作。
    void close();
    // 返回是否有尚未实际退出的回合。
    bool busy() const noexcept { return static_cast<bool>(active_); }
    // 判断已经停止接纳且后台调用真正退出。
    bool closed() const noexcept { return !*accepting_ && !active_; }
    // 返回宿主签发的独立会话身份。
    runtime::DomainId id() const noexcept { return id_; }
    // 返回已提交消息数；文本失败保留原历史，工具事实可在最终回答前独立提交。
    std::size_t message_count() const noexcept { return history_->size(); }
    // 返回失去局部协调器但仍在真实收尾的运行数量。
    static std::size_t retained() noexcept;
    // 在原宿主线程重试保留运行，实际退出前不释放工作责任。
    static std::size_t retry_retained();
  private:
    // 构造不可变会话事实并返回独立接纳回执，任何观察失败都留在通知边界。
    events::ObservationReceipt notify(std::string_view kind, std::uint64_t turn) noexcept;
    // 回收无客户端持有的已退出回合，保留结果继续计入槽容量。
    void collect() noexcept;
    model::Config config_;
    std::shared_ptr<events::BroadcastChannel> events_;
    std::shared_ptr<const runtime::StructDescriptor> descriptor_;
    const runtime::DomainId id_;
    std::uint64_t turn_ = 0;
    std::vector<std::shared_ptr<TurnRequest>> records_;
    std::unique_ptr<RunLoop> active_;
    std::shared_ptr<const MessageHistory> history_ = std::make_shared<const MessageHistory>();
    const model::Backend backend_;
    std::shared_ptr<bool> accepting_ = std::make_shared<bool>(true);
    bool working_ = false;
};
}
