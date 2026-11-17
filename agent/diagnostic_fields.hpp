#pragma once
#include <dafeiyu/diagnostic_snapshot.hpp>
#include <concepts>
#include <type_traits>

namespace dfy::agent::diagnostic_detail {
template<class E> struct EnumNames;
// 枚举只使用明确文本名称；不依赖内部序号成为持久协议。
#define DFY_NAMES(Type, ...) template<> struct EnumNames<Type> { static constexpr std::string_view values[] = {__VA_ARGS__}; }
DFY_NAMES(runtime::OperationState,"pending","succeeded","failed","cancelled");
DFY_NAMES(runtime::CancelReason,"none","user","owner_stop","deadline");
DFY_NAMES(runtime::ValueKind,"boolean","int64","uint64","float64","string","bytes","structure","null","array","map","enumeration","tagged_union");
DFY_NAMES(model::CallOutcome,"pending","returned","threw");
DFY_NAMES(ToolEffect,"not_applied","applied","unknown");
DFY_NAMES(ToolError,"none","failed","unavailable","invalid_result","cancel_failed","read_failed","release_failed","cancelled","deadline");
DFY_NAMES(TraceNodeKind,"run","step","attempt","model","tool","control");
DFY_NAMES(TraceFactKind,"accepted","model_submitted","model_exited","tool_candidate","tool_changed","confirmation_changed","policy_decision","history_committed","cancel_requested","settled","exited");
DFY_NAMES(PolicyReason,"none","invalid_context","stale_goal","stale_policy","cancelled","deadline","model_limit","tool_limit","source_denied","confirmation_required","work_pending","invalid_result","no_progress","policy_failed");
DFY_NAMES(ControlPoint,"model_request","tool_start","tool_result");
DFY_NAMES(PolicyVerdict,"allow","deny","wait");
DFY_NAMES(ConfirmationState,"waiting","approved","denied","cancelled","expired","invalidated","consumed");
DFY_NAMES(ObservationTrust,"external_data","host_verified");
DFY_NAMES(RunOrigin,"user_request","observation_rule");
DFY_NAMES(ModelPurpose,"direct","plan_generation","plan_step");
DFY_NAMES(TurnFailure,"none","model","history_commit","model_response","control","tool_execution","model_request");
DFY_NAMES(PlanStepState,"pending","running","completed","failed","cancelled","skipped");
DFY_NAMES(PlanWait,"none","work","exit","verification","stopped");
DFY_NAMES(ObservationMatch,"satisfied","not_satisfied","unknown");
DFY_NAMES(ObservationEvidenceIssue,"none","stale","wrong_object","untrusted","not_resampled","schema_changed","missing_field","type_changed");
DFY_NAMES(events::ObservationStatus,"accepted","overloaded","invalid","failed");
#undef DFY_NAMES
template<class T> struct Optional : std::false_type {};
template<class T> struct Optional<std::optional<T>> : std::true_type { using item = T; };
template<class T> struct Vector : std::false_type {};
template<class T> struct Vector<std::vector<T>> : std::true_type { using item = T; };
// 只为允许导出的纯值声明字段，没有 Config、Message、凭证或原生工作的映射。
#define DFY_FIELDS(Type, ...) template<class V,class F> requires std::same_as<std::remove_cv_t<V>,Type> void fields(V& v,F&& f) { __VA_ARGS__ }
// 映射完整运行身份。
DFY_FIELDS(RunId, f("session",v.session); f("sequence",v.sequence);)
// 映射原运行内步骤身份。
DFY_FIELDS(StepId, f("run",v.run); f("sequence",v.sequence);)
// 映射原步骤的尝试身份。
DFY_FIELDS(AttemptId, f("step",v.step); f("sequence",v.sequence);)
// 映射计划版本，不包含目标正文。
DFY_FIELDS(PlanBinding, f("run",v.run); f("goal_revision",v.goal_revision); f("plan_revision",v.plan_revision);)
// 映射计划步骤键，不导出步骤定义正文。
DFY_FIELDS(PlanStepKey, f("plan",v.plan); f("step",v.step);)
// 映射观察对象的公开标识与代次。
DFY_FIELDS(ObservationObject, f("id",v.id); f("generation",v.generation);)
// 映射无载荷观察来源关联。
DFY_FIELDS(ObservationInputFact, f("source",v.source); f("provider_instance",v.provider_instance); f("object",v.object); f("revision",v.revision); f("trust",v.trust);)
// 映射确定性判断，不含条件或期望值。
DFY_FIELDS(ObservationAssessment, f("match",v.match); f("issue",v.issue); f("source",v.source); f("revision",v.revision);)
// 映射显式控制决定。
DFY_FIELDS(PolicyDecision, f("verdict",v.verdict); f("reason",v.reason);)
// 映射原模型提供的可选用量。
DFY_FIELDS(model::Usage, f("input_tokens",v.input_tokens); f("output_tokens",v.output_tokens);)
// 映射公开工具版本，未知值保留为空。
DFY_FIELDS(ToolTraceVersion, f("id",v.id); f("contract",v.contract); f("instance",v.instance); f("plugin",v.plugin); f("schema",v.schema);)
// 映射允许公开的版本标签，不读取整个配置。
DFY_FIELDS(TraceVersions, f("application_commit",v.application_commit); f("build",v.build); f("prompt_template",v.prompt_template); f("provider",v.provider); f("model",v.model); f("model_profile",v.model_profile); f("policy",v.policy); f("tools",v.tools); f("public_model_config",v.public_model_config); f("execution_policy",v.execution_policy); f("control_extension",v.control_extension);)
// 映射文件命名空间及原运行。
DFY_FIELDS(TraceKey, f("namespace",v.name_space); f("run",v.run);)
// 映射节点路径，不包含操作句柄。
DFY_FIELDS(TraceNodeId, f("run",v.run); f("kind",v.kind); f("step",v.step); f("attempt",v.attempt); f("local",v.local);)
// 映射原状态和相对时间，缺失时刻保持空值。
DFY_FIELDS(TraceNode, f("id",v.id); f("parent",v.parent); f("result",v.result); f("exit",v.exit); f("submitted_ns",v.submitted_ns); f("accepted_ns",v.accepted_ns); f("call_started_ns",v.call_started_ns); f("call_finished_ns",v.call_finished_ns); f("settled_ns",v.settled_ns); f("observed_exit_ns",v.observed_exit_ns); f("call_outcome",v.call_outcome); f("effect",v.effect); f("error",v.error); f("fault",v.fault); f("usage",v.usage); f("accepted",v.accepted); f("related",v.related); f("before",v.before); f("after",v.after); f("confirmation",v.confirmation);)
// 映射固定大小事实；observed 已由边界转换为同文件相对纳秒。
DFY_FIELDS(TraceFact, f("revision",v.revision); f("subject",v.subject); f("kind",v.kind); f("observed_ns",v.observed); f("result",v.result); f("exit",v.exit); f("cancellation",v.cancellation); f("stop_reason",v.stop_reason); f("model_outcome",v.model_outcome); f("effect",v.effect); f("error",v.error); f("fault",v.fault); f("control_point",v.control_point); f("decision",v.decision); f("confirmation",v.confirmation); f("source_revision",v.source_revision); f("related",v.related); f("history_messages",v.history_messages);)
// 映射保留窗口及缺口，不把分页当成完整历史。
DFY_FIELDS(TraceDelta, f("run",v.run); f("requested_after",v.requested_after); f("oldest_revision",v.oldest_revision); f("latest_revision",v.latest_revision); f("gap",v.gap); f("more",v.more); f("projection_failed",v.projection_failed); f("facts",v.facts);)
// 映射通知身份，没有事件载荷。
DFY_FIELDS(events::EventId, f("domain",v.domain); f("sequence",v.sequence);)
// 映射通知接纳回执，不承诺观察者已经消费。
DFY_FIELDS(events::ObservationReceipt, f("status",v.status); f("event",v.event);)
// 映射原开始与终态通知回执。
DFY_FIELDS(TurnNotifications, f("started",v.started); f("finished",v.finished);)
// 映射每次模型前控制的原版本，不复制输入或输出。
DFY_FIELDS(ModelContextSnapshot, f("attempt",v.attempt); f("goal_revision",v.goal_revision); f("plan_revision",v.plan_revision); f("purpose",v.purpose); f("plan_step",v.plan_step); f("observation_binding",v.observation_binding); f("observations_current",v.observations_current);)
// 映射观察更新的原接纳结果，不能重建刷新请求。
DFY_FIELDS(ObservationRebindReceipt, f("request",v.request); f("previous_binding",v.previous_binding); f("binding",v.binding); f("step",v.step); f("observations",v.observations); f("after_model_requests",v.after_model_requests);)
// 映射时间线值，原始单调时钟起点有意不进入文件。
DFY_FIELDS(RunTimeline, f("key",v.key); f("versions",v.versions); f("nodes",v.nodes); f("delta",v.delta); f("notifications",v.notifications); f("failure",v.failure); f("stop_reason",v.stop_reason); f("source",v.source); f("goal_revision",v.goal_revision); f("observations",v.observations); f("model_contexts",v.model_contexts); f("initial_observation_binding",v.initial_observation_binding); f("observation_rebinds",v.observation_rebinds);)
// 映射计划进度，仅保留证据存在性。
DFY_FIELDS(PlanStepDiagnostic, f("key",v.key); f("state",v.state); f("waiting",v.waiting); f("execution",v.execution); f("result",v.result); f("exit",v.exit); f("has_evidence",v.has_evidence);)
// 映射一个目标的计划版本和停止状态。
DFY_FIELDS(PlanDiagnostic, f("binding",v.binding); f("stopped",v.stopped); f("steps",v.steps);)
// 映射反馈基线和末次判断，不导出期望值或条件字段。
DFY_FIELDS(PlanFeedbackSnapshot, f("request",v.request); f("step",v.step); f("issued_attempt",v.issued_at); f("completed_tool",v.completed_tool); f("baseline",v.baseline); f("expected_type",v.expected_type); f("latest",v.latest); f("checks",v.checks); f("verified",v.verified); f("checks_saturated",v.checks_saturated);)
// 映射整个只读诊断值，不包含任何可执行对象。
DFY_FIELDS(DiagnosticSnapshot, f("timeline",v.timeline); f("plans",v.plans); f("feedback",v.feedback);)
#undef DFY_FIELDS
}
