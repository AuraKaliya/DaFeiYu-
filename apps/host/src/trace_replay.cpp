#include "trace_commands.hpp"
#include "trace_model.hpp"
#include "trace_read.hpp"
#include <dafeiyu/digest.hpp>
#include <dafeiyu/model_protocol.hpp>
#include <dafeiyu/run_loop.hpp>
#include <dafeiyu/session.hpp>
#include <algorithm>
#include <ostream>
#include <thread>

namespace dfy::app {
namespace {
using namespace trace_format;
using agent::ToolResult;

// 解码日志使用的固定标签，未知标签不能降级为默认值。
template<typename Enum, std::size_t Count>
Enum tagged(const Json& field, const std::array<const char*,Count>& names) {
    const auto name = field.get<std::string>();
    const auto found = std::find(names.begin(),names.end(),name);
    if (found == names.end()) throw std::invalid_argument("replay: unknown protocol label");
    return static_cast<Enum>(found-names.begin());
}

// 还原包含配对身份和完整调用的统一消息。
model::Message decode_message(const Json& value) {
    model::Message message{tagged<model::Role>(value.at("role"),roles),value.at("content"),{},value.at("toolCallId")};
    if (value.contains("reasoningContent")) message.reasoning_content = value.at("reasoningContent").get<std::string>();
    for (const auto& call : value.at("toolCalls"))
        message.tool_calls.push_back({call.at("id"),call.at("name"),call.at("arguments")});
    return message;
}

// 还原原响应正文、结束标签与可选用量。
model::Response decode_response(const Json& value) {
    model::Response response{decode_message(value.at("message")),tagged<model::FinishReason>(value.at("finish"),finishes)};
    if (const auto& usage = value.at("usage"); !usage.is_null()) {
        response.usage = model::Usage{usage.at("inputTokens"),usage.at("outputTokens")};
        if (usage.contains("cacheHitTokens")) response.usage->cache_hit_tokens = usage.at("cacheHitTokens").get<std::uint64_t>();
        if (usage.contains("cacheMissTokens")) response.usage->cache_miss_tokens = usage.at("cacheMissTokens").get<std::uint64_t>();
    }
    return response;
}

// 匹配原工具步骤和配对信息，不使用新进程生成的工作号。
bool matches_call(const Json& record, agent::AttemptId attempt, const model::ToolCall& call) {
    return record.at("step") == attempt.step.sequence && record.at("attempt") == attempt.sequence &&
        record.at("callId") == call.id && record.at("name") == call.name && record.at("arguments") == call.arguments;
}

struct RecordedModel {
    Json input, exit;
    std::optional<model::Response> response, rewritten;
    std::size_t rewritten_arguments = 0;
    bool rewrite_observed = false;
    Json evidence;
    std::optional<std::string> wire_digest;
    bool thinking = false;
};

// 读取原件随附的时间，缺失保持未知，在重放期间不依赖本机当前时钟。
std::optional<std::int64_t> original_time(const Json& evidence) {
    if (!evidence.contains("eventUtcMs")) return {};
    const auto& time = evidence.at("eventUtcMs");
    if (!time.is_number_integer() || (time.is_number_unsigned() && time.get<std::uint64_t>()>INT64_MAX))
        throw std::invalid_argument("replay: invalid event time");
    return time.get<std::int64_t>();
}

// 核对程序产生的来源，再以原事件时间重建供应商字段，早期轨迹仍能核对统一请求。
// 步骤：比对原文与配对 → 核对来源并还原原时间 → 重新编码与比较指纹。
bool matches_request(const model::Request& actual, const RecordedModel& entry) {
    if (encode_request(actual)!=entry.input) return false;
    if (entry.evidence.is_null()) return !entry.wire_digest;
    auto request = actual;
    for (std::size_t i=0; i<request.messages.size(); ++i) {
        auto& message = request.messages[i];
        const auto& evidence = entry.evidence.at(i);
        if (model::source_name(message.source)!=evidence.at("source").get_ref<const std::string&>()) return false;
        message.event_utc_ms = original_time(evidence);
    }
    return !entry.wire_digest || platform::sha256(model::encode_request_fields(request,entry.thinking))==*entry.wire_digest;
}
struct RecordedTool { Json exit; bool invoked = false, observed = false; };
struct RecordedInput {
    std::string text;
    std::size_t after_step;
    std::optional<std::string> state;
    std::uint64_t model_step = 0;
};
struct RecordedRun {
    std::vector<RecordedModel> models;
    std::vector<RecordedTool> tools;
    std::vector<RecordedInput> inputs;
    Json terminal;
    std::optional<agent::PolicyDecision> next_model_control;
};
constexpr std::array turn_failures{"none","model","history_commit","model_response","control","tool_execution","model_request"};
constexpr std::array policy_reasons{"none","invalid_context","stale_goal","stale_policy","cancelled","deadline","model_limit","tool_limit",
    "source_denied","confirmation_required","work_pending","invalid_result","no_progress","policy_failed"};

// 保留原模型输出，仅接纳 Hook 可修改的正文与逐项参数。
// 步骤：校验同一调用退出 → 复制允许改写的字段 → 对照其余身份与元数据。
void record_rewrite(RecordedModel& entry, const Json& row) {
    if (!entry.response || entry.rewritten || entry.exit.at("request") != row.at("request") ||
        row.at("callOutcome") != "returned" || row.at("result") != "succeeded")
        throw std::invalid_argument("replay: invalid model rewrite identity");
    const auto& recorded = row.at("output");
    const auto replacement = decode_response(recorded);
    auto effective = *entry.response;
    if (effective.message.tool_calls.size() != replacement.message.tool_calls.size())
        throw std::invalid_argument("replay: model rewrite changed tool count");
    effective.message.content = replacement.message.content;
    for (std::size_t i = 0; i < effective.message.tool_calls.size(); ++i)
        effective.message.tool_calls[i].arguments = replacement.message.tool_calls[i].arguments;
    if (encode_response(effective) != recorded) throw std::invalid_argument("replay: model rewrite changed immutable fields");
    entry.rewritten = std::move(effective);
}

// 只有原诊断明确记录的终止门才能作为离线输入，避免从终态倒推出不存在的控制事件。
bool model_control_after(const Json& timeline, std::size_t expected_step, std::string_view verdict) {
    const auto& delta = timeline.at("delta");
    if (delta.at("gap") == true || delta.at("more") == true || delta.at("projection_failed") == true) return false;
    return std::any_of(delta.at("facts").begin(),delta.at("facts").end(),[&](const Json& event) {
        return event.at("kind") == "policy_decision" && event.at("control_point") == "model_request" &&
            event.at("subject").at("step") == expected_step && event.at("decision").at("verdict").get<std::string>() == verdict &&
            event.at("decision").at("reason") == timeline.at("stop_reason");
    });
}

// 只装入有确定边界的直接运行，等待人为输入及中断的运行留给原日志审阅。
// 步骤：按原身份收取事件 → 配对模型输入及退出 → 验证终态和可重跑边界。
RecordedRun recorded_run(const std::filesystem::path& file, agent::RunId run) {
    RecordedRun result;
    read_trace(file,[&](const Json& row) {
        if (!matches_run(row,run)) return;
        const auto& kind = row.at("kind");
        if (kind == "model.prepared") {
            if (result.models.size() == 64 || row.at("step") != result.models.size()+1 || row.at("attempt") != 1 ||
                (!result.models.empty() && result.models.back().exit.is_null()))
                throw std::invalid_argument("replay: unsupported model schedule");
            auto& entry = result.models.emplace_back();
            entry.input = row.at("input");
            if (row.contains("messageEvidence")) {
                entry.evidence = row.at("messageEvidence");
                if (!entry.evidence.is_array() || entry.evidence.size()!=entry.input.at("messages").size())
                    throw std::invalid_argument("replay: message evidence count differs");
            }
            if (row.contains("wireSha256")) {
                if (entry.evidence.is_null()) throw std::invalid_argument("replay: missing message evidence");
                entry.wire_digest = row.at("wireSha256").get<std::string>();
                entry.thinking = row.at("thinking").get<bool>();
            }
        } else if (kind == "model.exited") {
            if (result.models.empty() || row.at("step") != result.models.size() || row.at("attempt") != 1 ||
                !result.models.back().exit.is_null() ||
                (row.at("callOutcome") != "threw" && !(row.at("callOutcome") == "returned" && row.contains("output"))) ||
                row.at("result") == "cancelled")
                throw std::invalid_argument("replay: unsupported model exit");
            result.models.back().exit = row;
            if (row.contains("output")) result.models.back().response = decode_response(row.at("output"));
        } else if (kind == "tool.exited") {
            if (result.tools.size() == 64 || row.contains("confirmation") || row.at("fault") != "none" ||
                row.at("result") == "cancelled")
                throw std::invalid_argument("replay: unsupported tool exit");
            result.tools.push_back({row});
        } else if (kind == "model.rewritten") {
            if (result.models.empty() || row.at("step") != result.models.size() || row.at("attempt") != 1)
                throw std::invalid_argument("replay: unexpected model rewrite");
            record_rewrite(result.models.back(),row);
        } else if (kind == "dialogue.input_accepted") {
            if (result.models.empty() || !result.terminal.is_null() || result.inputs.size() >= agent::turn_input_count_limit ||
                row.at("message") != result.inputs.size()+1) throw std::invalid_argument("replay: invalid dialogue input identity");
            const auto& text = row.at("body").get_ref<const std::string&>();
            if (text.size() > agent::turn_input_byte_limit) throw std::length_error("replay: dialogue input too large");
            result.inputs.push_back({text,result.models.size(),{}});
        } else if (kind == "dialogue.input_settled") {
            const auto message = row.at("message").get<std::uint64_t>();
            if (!message || message > result.inputs.size() || !result.terminal.is_null()) throw std::invalid_argument("replay: unknown dialogue input");
            auto& input = result.inputs[static_cast<std::size_t>(message-1)];
            if (input.state) throw std::invalid_argument("replay: duplicate dialogue input settlement");
            input.state = row.at("state").get<std::string>();
            input.model_step = row.at("modelStep").get<std::uint64_t>();
        } else if (kind == "run.exited") {
            if (!result.terminal.is_null()) throw std::invalid_argument("replay: duplicate run exit");
            result.terminal = row;
        }
    });
    if (result.terminal.is_null() || result.models.empty() || result.models.back().exit.is_null())
        throw std::invalid_argument("replay: complete run with model input and exit required");
    if (!result.terminal.contains("diagnostic"))
        throw std::invalid_argument("replay: original run diagnostics missing");
    const auto& diagnostic = result.terminal.at("diagnostic").at("data");
    const auto& timeline = diagnostic.at("timeline");
    const auto reason = tagged<agent::PolicyReason>(timeline.at("stop_reason"),policy_reasons);
    if ((result.terminal.at("result") != "succeeded" && result.terminal.at("result") != "failed") ||
        !diagnostic.at("plans").empty() || !timeline.at("observations").empty() ||
        (reason != agent::PolicyReason::none && reason != agent::PolicyReason::source_denied &&
            reason != agent::PolicyReason::work_pending) || timeline.at("source") == "observation_rule")
        throw std::invalid_argument("replay: run requires control inputs outside a direct replay");
    if (reason == agent::PolicyReason::source_denied) {
        if (timeline.at("failure") != "control" || !model_control_after(timeline,result.models.size()+1,"deny"))
            throw std::invalid_argument("replay: original model boundary decision unavailable");
        result.next_model_control = agent::PolicyDecision{agent::PolicyVerdict::deny,reason};
    } else if (reason == agent::PolicyReason::work_pending) {
        if (result.terminal.at("result") != "succeeded" || !model_control_after(timeline,result.models.size()+1,"wait"))
            throw std::invalid_argument("replay: original yield boundary unavailable");
        result.next_model_control = agent::PolicyDecision{agent::PolicyVerdict::wait,reason};
    }
    return result;
}

// 离线复用原 Hook 的输出，并由正式模型改写阶段检查字段边界及产出。
// 步骤：依据步骤绑定原改写 → 按调用顺序返回正文和参数 → 核对正式协调器发出的有效响应。
void bind_rewrites(RecordedRun& original, agent::RunOptions& options, std::string& difference) {
    // 根据原模型步骤及有序字段返回同一份改写值。
    // 步骤：匹配文本边界 → 核对原字段 → 返回对应的有效值或报告差异。
    options.text_hook = [&](agent::TextHookPoint point, const agent::PolicyContext& context, std::string_view field,
        std::string_view value, std::size_t) -> std::optional<std::string> {
        const auto step = context.attempt.step.sequence;
        if (step == 0 || step > original.models.size() ||
            (point != agent::TextHookPoint::model_response && point != agent::TextHookPoint::tool_arguments)) return {};
        auto& entry = original.models[step-1];
        if (!entry.rewritten) return {};
        const auto& raw = entry.response->message;
        const auto& effective = entry.rewritten->message;
        if (point == agent::TextHookPoint::model_response && field == "content" && value == raw.content)
            return effective.content;
        if (point == agent::TextHookPoint::tool_arguments && entry.rewritten_arguments < raw.tool_calls.size()) {
            const auto index = entry.rewritten_arguments++;
            if (field == raw.tool_calls[index].name && value == raw.tool_calls[index].arguments)
                return effective.tool_calls[index].arguments;
        }
        difference = "model rewrite input differs at step "+std::to_string(step);
        return {};
    };
    // 以运行时产出的有效响应逐项核对已记录的改写。
    options.model_capture = [&](const agent::ModelCapture& capture) {
        if (capture.phase != agent::ModelCapturePhase::rewritten) return;
        const auto step = capture.attempt.step.sequence;
        if (step > 0 && step <= original.models.size()) {
            auto& entry = original.models[step-1];
            if (entry.rewritten && !entry.rewrite_observed && entry.rewritten_arguments == entry.rewritten->message.tool_calls.size() &&
                capture.output && encode_response(*capture.output) == encode_response(*entry.rewritten)) {
                entry.rewrite_observed = true;
                return;
            }
        }
        difference = "model rewrite output differs at step "+std::to_string(step);
    };
}
}

// 用固定证据驱动正式运行协调器，比较实际送模上下文、工具回执与最终结果。
// 步骤：恢复首轮历史和工具定义 → 绑定纯离线替身 → 推进正式回合 → 核对首个差异和原终态。
void replay_trace(const std::filesystem::path& file, agent::RunId run, std::ostream& output) {
    using runtime::OperationState;
    auto original = recorded_run(file,run);
    const auto& first = original.models.front().input;
    auto history = std::make_shared<agent::MessageHistory>();
    const auto& evidence = original.models.front().evidence;
    for (const auto& item : first.at("messages")) {
        auto message = decode_message(item);
        if (!evidence.is_null()) {
            const auto& origin = evidence.at(history->size());
            const auto source = model::parse_source(origin.at("source").get_ref<const std::string&>());
            if (!source) throw std::invalid_argument("replay: unknown message source");
            message.source = *source;
            message.event_utc_ms = original_time(origin);
        }
        history->push_back(std::move(message));
    }
    if (history->empty() || history->back().role != model::Role::user)
        throw std::invalid_argument("replay: final initial user message required");
    agent::RunOptions options;
    if (!evidence.is_null()) {
        options.prompt_source = history->back().source;
        options.prompt_utc_ms = history->back().event_utc_ms;
    }
    auto prompt = std::move(history->back().content);
    history->pop_back();

    std::string tool_difference, model_difference, rewrite_difference;
    std::size_t model_index = 0;
    agent::ToolCatalog catalog;
    options.accept_user_input = !original.inputs.empty();
    constexpr std::array origins{"user_request","observation_rule","mailbox"};
    options.origin = tagged<agent::RunOrigin>(original.terminal.at("diagnostic").at("data").at("timeline").at("source"),origins);
    if (std::any_of(original.models.begin(),original.models.end(),[](const auto& entry) { return entry.rewritten.has_value(); }))
        bind_rewrites(original,options,rewrite_difference);
    bool model_control_seen = false;
    if (original.next_model_control) {
        // 重用记录中的宿主控制输入，重跑时仍由正式协调器产生终止和退出。
        options.extension = [&](agent::ControlPoint point, const agent::PolicyContext& context) {
            if (point == agent::ControlPoint::model_request && context.attempt.step.sequence == original.models.size()+1) {
                model_control_seen = true;
                return *original.next_model_control;
            }
            return agent::PolicyDecision{agent::PolicyVerdict::allow,agent::PolicyReason::none};
        };
    }
    std::vector<std::string> ids;
    for (const auto& tool : first.at("tools")) {
        model::ToolDefinition definition{tool.at("name"),tool.at("description"),tool.at("parameters")};
        ids.push_back("replay/"+definition.name);
        catalog.add({ids.back(),runtime::next_domain(),1,std::move(definition)},
        // 按原步骤和精确参数兑现一次固定结果，永不接触游戏执行器。
        [&](const agent::ToolInvocation& invocation) {
            for (auto& candidate : original.tools) {
                const auto& row = candidate.exit;
                if (candidate.invoked || row.at("accepted") != true || !matches_call(row,invocation.attempt,invocation.call)) continue;
                candidate.invoked = true;
                return agent::completed_tool(ToolResult{row.at("succeeded"),tagged<agent::ToolEffect>(row.at("effect"),effects),
                    row.at("output"),tagged<agent::ToolError>(row.at("error"),faults)});
            }
            if (tool_difference.empty()) tool_difference = "unexpected tool invocation "+invocation.call.id;
            throw std::runtime_error("replay: unexpected tool invocation");
        });
    }
    if (!ids.empty()) options.tools = catalog.snapshot(ids);
    options.tool_capture = [&](const agent::ToolCapture& capture) {
        if (capture.phase != agent::ToolCapturePhase::exited) return;
        for (auto& candidate : original.tools) {
            const auto& row = candidate.exit;
            if (candidate.observed || row.at("accepted") != capture.candidate.accepted ||
                !matches_call(row,capture.candidate.attempt,capture.call)) continue;
            const auto* request = capture.request;
            const auto status = request ? request->operation()->state() : capture.candidate.result;
            const auto outcome = request ? request->outcome() :
                ToolResult{false,capture.candidate.effect,capture.candidate.rejection,capture.candidate.error};
            if (row.at("result") != states.at(static_cast<std::size_t>(status)) || row.at("succeeded") != outcome.succeeded ||
                row.at("effect") != effects.at(static_cast<std::size_t>(outcome.effect)) ||
                row.at("error") != faults.at(static_cast<std::size_t>(outcome.error)) || row.at("output") != outcome.output) break;
            candidate.observed = true;
            return;
        }
        if (tool_difference.empty()) tool_difference = "tool exit differs for "+capture.call.id;
    };
    const auto& service = original.models.front().exit;
    agent::RunLoop loop(run,model::Config{service.at("provider"),{},service.at("model")},
        // 后台仅对照完整请求和取回原结果。主线程等模型退出后才访问比对状态。
        model::Backend{[&](const model::Config&,const model::Request& input) {
            const auto index = model_index++;
            if (index >= original.models.size() || !matches_request(input,original.models[index])) {
                model_difference = "model request "+std::to_string(index+1)+" differs";
                throw std::runtime_error("replay: input differs");
            }
            const auto& exit = original.models[index].exit;
            if (exit.at("callOutcome") == "threw") throw std::runtime_error("replay: recorded model failure");
            return *original.models[index].response;
        }},std::move(history),std::move(prompt),std::move(options));
    const auto request = loop.request();
    std::size_t next_input = 0;
    bool admission_differs = false;
    const auto until = std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (request->exit()->state() == OperationState::pending && std::chrono::steady_clock::now() < until) {
        // 只在原收件所处的请求边界交给正式循环，工具仍按自己的结果与退出推进。
        while (next_input < original.inputs.size() && original.inputs[next_input].after_step == request->diagnostics().model_requests) {
            const auto receipt = loop.append_input(original.inputs[next_input].text);
            if (receipt.state != agent::InputAdmission::accepted || receipt.message != next_input+1) {
                admission_differs = true;
                next_input = original.inputs.size();
                request->request_cancel(runtime::CancelReason::owner_stop);
                break;
            }
            ++next_input;
        }
        loop.advance();
        if (request->exit()->state() == OperationState::pending) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    loop.wait_model();
    if (request->exit()->state() == OperationState::pending) throw std::runtime_error("replay: offline coordinator did not exit");
    if (!model_difference.empty()) throw std::runtime_error("replay: "+model_difference);
    if (!rewrite_difference.empty()) throw std::runtime_error("replay: "+rewrite_difference);
    if (!tool_difference.empty()) throw std::runtime_error("replay: "+tool_difference);
    if (admission_differs || request->inputs().size() != original.inputs.size()) throw std::runtime_error("replay: dialogue input admission differs");
    for (std::size_t i=0; i<original.inputs.size(); ++i) {
        const auto& actual = request->inputs()[i];
        const auto& recorded = original.inputs[i];
        if (!recorded.state || *recorded.state != agent::input_state_name(actual.state) || recorded.model_step != actual.model_step)
            throw std::runtime_error("replay: dialogue input settlement differs");
    }
    if (model_index != original.models.size() ||
        model_control_seen != original.next_model_control.has_value() ||
        std::any_of(original.models.begin(),original.models.end(),[](const auto& entry) {
            return entry.rewritten.has_value() != entry.rewrite_observed;
        }) ||
        std::any_of(original.tools.begin(),original.tools.end(),[](const auto& entry) {
            return !entry.observed || entry.invoked != entry.exit.at("accepted");
        })) throw std::runtime_error("replay: recorded model or tool events were not consumed");
    const auto state = request->operation()->state();
    const auto& timeline = original.terminal.at("diagnostic").at("data").at("timeline");
    if (original.terminal.at("result") != states.at(static_cast<std::size_t>(state)) ||
        timeline.at("failure") != turn_failures.at(static_cast<std::size_t>(request->diagnostics().failure)) ||
        timeline.at("stop_reason") != policy_reasons.at(static_cast<std::size_t>(request->diagnostics().stop_reason)) ||
        (state == OperationState::succeeded && original.terminal.at("answer") != request->answer()))
        throw std::runtime_error("replay: final result differs");
    output << "Replay matched session " << run.session << " run " << run.sequence << ": " << model_index
        << " model requests, " << original.tools.size() << " tool exits, " << states.at(static_cast<std::size_t>(state)) << '\n';
}
}
