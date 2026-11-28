#pragma once
#include <dafeiyu/execution_policy.hpp>
#include <dafeiyu/confirmation.hpp>
#include <dafeiyu/trace_identity.hpp>
#include <dafeiyu/observation_input.hpp>
#include <dafeiyu/plan_definition.hpp>
#include <dafeiyu/action_observations.hpp>
#include <dafeiyu/run_capture.hpp>
#include <dafeiyu/context_status.hpp>
#include <dafeiyu/decision_status.hpp>
#include <dafeiyu/turn_input.hpp>
#include <dafeiyu/prompt_rules.hpp>
#include <dafeiyu/context_capacity.hpp>

namespace dfy::agent {
class ObservationController;
// 由观察控制器在条件满足时签发，作用域限于原行动观察绑定和规则修订。
class ObservationActionAuthorization {
  public:
    // 检查原行动依据，零或重采样后的不同绑定都不具备此授权。
    bool matches(runtime::DomainId binding) const noexcept { return binding && binding == binding_; }
    // 返回宿主声明的规则修订，供运行记录实际参与的控制版本。
    std::uint64_t revision() const noexcept { return revision_; }
  private:
    friend class ObservationController;
    // 固定已通过本控制器前置条件的原绑定和版本。
    ObservationActionAuthorization(runtime::DomainId binding, std::uint64_t revision) : binding_(binding),revision_(revision) {}
    runtime::DomainId binding_;
    std::uint64_t revision_;
};
enum class TextHookPoint { input, model_request, model_response, tool_arguments, reply };
using TextHook = std::function<std::optional<std::string>(TextHookPoint,const PolicyContext&,std::string_view,std::string_view,std::size_t)>;
// 由入口授权的本会话视图；采样只在原宿主线程的决策边界执行。
struct DecisionStatusOptions {
    std::string binding;
    std::function<std::vector<DecisionStatusSource>()> sample;
    bool allow_read = false;
    std::optional<ToolSnapshot> read_authority;
    bool include_tool_counts = false;
};
// 只供正式请求使用的观察批次，游标由来源按角色签发。
struct DecisionObservationBatch { std::uint64_t cursor; std::string text; };
struct DecisionObservationFeed {
    std::shared_ptr<std::uint64_t> submitted_cursor;
    std::function<std::optional<DecisionObservationBatch>(std::uint64_t,AttemptId,ToolTime)> read;
};
// 宿主固定的运行选项，模型输出不得修改目标版本、权限或限额。
struct RunOptions {
    std::optional<ToolSnapshot> tools;
    std::optional<PlanLimits> planning;
    std::size_t goal_update_limit = 4;
    std::size_t observation_rebind_limit = 16;
    PolicySettings policy;
    std::uint64_t goal_revision = 1;
    RunOrigin origin = RunOrigin::user_request;
    std::chrono::milliseconds timeout{120000};
    std::optional<ToolTime> deadline;
    std::vector<std::shared_ptr<const ObservationRecord>> observations;
    std::optional<ActionObservations> action_observations;
    std::optional<ObservationActionAuthorization> observation_action;
    std::size_t no_progress_limit = 3;
    ToolClock now = std::chrono::steady_clock::now;
    PolicyExtension extension;
    TextHook text_hook;
    // 结构化信件由宿主包装，协议工具保留模型传回的原参数。
    bool preserve_prompt = false;
    std::string immutable_tool_arguments;
    ModelCaptureSink model_capture;
    TurnExitSink exit_capture;
    TurnExitSink exit_observer;
    ToolCaptureSink tool_capture;
    std::optional<ConfirmationCaller> confirmation_caller;
    std::size_t confirmation_limit = 16;
    std::size_t trace_capacity = 256;
    RunVersionLabels version_labels;
    std::shared_ptr<runtime::Budget> budget;
    // 宿主为本轮提供的角色引导，仅在构造模型请求时注入。
    std::string system_instructions;
    model::ThinkingPreference thinking = model::ThinkingPreference::inherit;
    // 仅由允许真人对话的宿主开启，当轮正文不能改变既有授权。
    bool accept_user_input = false;
    // 仅在同步采集回调中比较同一截止号的只读投影。
    bool compare_projection = false;
    // 状态基线由会话从已提交历史取得，新输入只能贡献当前绑定的事实。
    std::shared_ptr<const ContextStatus> context_status, previous_status;
    // 正式入口按绑定声明数据来源；存储与有效代次由 Session 在接纳时签发。
    std::optional<DecisionStatusOptions> decision_status;
    // 被动感知仅在本来就要发送的请求中读取，实际分派且正文入窗后才推进游标。
    std::optional<DecisionObservationFeed> decision_observations;
    std::shared_ptr<DecisionStatusStore> status_store;
    std::uint64_t status_epoch = 0;
    ToolCallCounts previous_tool_counts;
    bool replace_history = false;
    std::uint64_t evidence_epoch = 0;
    // 复合的信箱或插件输入由入口标记，直接对话和观察规则由运行起因确定。
    std::optional<model::MessageSource> prompt_source;
    // 原话的渠道时间可随准入传入，后续事件的 UTC 时钟独立于期限和预算。
    std::optional<std::int64_t> prompt_utc_ms;
    std::uint64_t prompt_message = 0;
    DialogueSummary summarize_dialogue;
    // 在完整请求通过容量复核后通知宿主保存本次使用的摘要检查点。
    std::function<void(const model::Request&)> save_dialogue_summary;
    MessageClock message_clock;
    // 宿主固定的输出去向及人格表达，按当前用途装配到正式系统段。
    std::optional<PromptRules> prompt_rules;
};
}
