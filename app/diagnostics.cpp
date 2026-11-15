#include "diagnostics.hpp"
#include <ostream>

namespace dfy::app {
namespace {
// 把观察接纳状态映射为稳定名称，accepted 仅说明已进入通道。
std::string_view observation_name(events::ObservationStatus status) noexcept {
    switch (status) {
    case events::ObservationStatus::accepted: return "accepted (queued only)";
    case events::ObservationStatus::overloaded: return "capacity exhausted";
    case events::ObservationStatus::invalid: return "invalid notification";
    case events::ObservationStatus::failed: return "notification failure";
    }
    return "unknown";
}
// 展示原业务操作的唯一状态，不由调用时间推测成功。
std::string_view operation_name(runtime::OperationState state) noexcept {
    switch (state) {
    case runtime::OperationState::pending: return "pending";
    case runtime::OperationState::succeeded: return "succeeded";
    case runtime::OperationState::failed: return "failed";
    case runtime::OperationState::cancelled: return "cancelled";
    }
    return "unknown";
}
// 展示首个取消来源，不把关闭请求描述为用户手动取消。
std::string_view cancellation_name(runtime::CancelReason reason) noexcept {
    switch (reason) {
    case runtime::CancelReason::none: return "none";
    case runtime::CancelReason::user: return "user";
    case runtime::CancelReason::owner_stop: return "shutdown";
    case runtime::CancelReason::deadline: return "deadline";
    }
    return "unknown";
}
// 解释失败发生的业务阶段，不输出可能包含请求内容的异常文本。
std::string_view failure_name(agent::TurnFailure failure) noexcept {
    switch (failure) {
    case agent::TurnFailure::none: return "none";
    case agent::TurnFailure::model: return "model";
    case agent::TurnFailure::history_commit: return "history commit";
    case agent::TurnFailure::model_response: return "model response";
    case agent::TurnFailure::control: return "execution control";
    case agent::TurnFailure::tool_execution: return "tool execution";
    case agent::TurnFailure::model_request: return "model submission";
    }
    return "unknown";
}
// 展示后台事实；调用返回或抛错均不等同于宿主退出确认。
std::string_view call_name(const model::CallDiagnostics& call) noexcept {
    if (call.outcome == model::CallOutcome::returned) return "returned";
    if (call.outcome == model::CallOutcome::threw) return "threw";
    return call.started ? "running" : "not started";
}
// 以模型提交为统一起点显示毫秒偏移，尚未发生的阶段显示为横线。
void write_time(std::ostream& output, std::string_view label, std::optional<agent::TurnDiagnostics::TimePoint> value,
    agent::TurnDiagnostics::TimePoint origin) {
    output << "  " << label << "=";
    if (value) output << std::chrono::duration_cast<std::chrono::milliseconds>(*value-origin).count();
    else output << '-';
    output << '\n';
}
// 显示接纳回执及事件身份，缺失回执表示该阶段尚未产生通知。
void write_receipt(std::ostream& output, std::string_view label, const std::optional<events::ObservationReceipt>& receipt) {
    output << "  " << label << "=";
    if (!receipt) output << "not emitted";
    else {
        output << observation_name(receipt->status);
        if (receipt->status == events::ObservationStatus::accepted) output << "; event=" << receipt->event.domain << ':' << receipt->event.sequence;
    }
    output << '\n';
}
}
// 通知失败只形成单独提示，成功接纳保持静默且不承诺观察者已经处理。
// 步骤：跳过已接纳回执 → 输出事件种类、回合身份和失败类别 → 明确问答结果独立。
void write_observation_warning(std::ostream& output, runtime::DomainId session, std::uint64_t turn,
    std::string_view kind, events::ObservationReceipt receipt) {
    if (receipt.status == events::ObservationStatus::accepted) return;
    output << "Warning: " << kind << " notification not accepted (" << observation_name(receipt.status)
        << "; session=" << session << ", turn=" << turn << "). Question result is unchanged.\n";
}
// 将固定大小诊断快照转换为本地查询结果，正文、密钥与异常文本不参与格式化。
// 步骤：展示关联身份与独立状态 → 输出调用阶段及时间线 → 展示两次通知接纳回执。
void write_turn_status(std::ostream& output, const agent::TurnDiagnostics& snapshot) {
    output << "Session=" << snapshot.session << ", turn=" << snapshot.turn << ", model request=" << snapshot.model.request << '\n'
        << "Question=" << operation_name(snapshot.result) << ", failure stage=" << failure_name(snapshot.failure) << '\n'
        << "Cancellation=" << cancellation_name(snapshot.cancellation) << '\n'
        << "Model call=" << call_name(snapshot.model) << ", exit="
        << (snapshot.exit == runtime::OperationState::succeeded ? "confirmed" : "waiting") << '\n';

    const auto end = snapshot.exited.value_or(std::chrono::steady_clock::now());
    output << "Elapsed ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(end-snapshot.model.submitted).count()
        << " (" << (snapshot.exited ? "final" : "active") << ")\nTimes in ms since model submission:\n";
    write_time(output,"accepted",snapshot.accepted,snapshot.model.submitted);
    write_time(output,"model started",snapshot.model.started,snapshot.model.submitted);
    write_time(output,"model finished",snapshot.model.finished,snapshot.model.submitted);
    write_time(output,"cancel requested",snapshot.cancel_requested,snapshot.model.submitted);
    write_time(output,"question settled",snapshot.settled,snapshot.model.submitted);
    write_time(output,"exit confirmed",snapshot.exited,snapshot.model.submitted);

    output << "Notifications (acceptance only):\n";
    write_receipt(output,"started",snapshot.notifications.started);
    write_receipt(output,"finished",snapshot.notifications.finished);
}
}
