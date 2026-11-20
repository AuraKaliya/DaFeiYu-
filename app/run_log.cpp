#include "run_log.hpp"
#include <dafeiyu/diagnostic_json.hpp>
#include <array>
#include <iostream>
#include <nlohmann/json.hpp>

namespace dfy::app {
namespace {
using Json = nlohmann::json;
constexpr std::array roles{"user","assistant","system","tool"};
constexpr std::array finishes{"stop","tool_calls","length","content_filter"};
constexpr std::array states{"pending","succeeded","failed","cancelled"};
constexpr std::array outcomes{"pending","returned","threw"};
constexpr std::array phases{"model.prepared","model.submitted","model.exited","model.rewritten"};
constexpr std::array tool_phases{"tool.candidate","tool.admitted","tool.exited"};
constexpr std::array effects{"not_applied","applied","unknown"};
constexpr std::array faults{"none","failed","unavailable","invalid_result","cancel_failed","read_failed","release_failed","cancelled","deadline"};

// 保留统一模型协议的消息顺序、工具请求和配对身份。
Json encode_message(const dfy::model::Message& message) {
    Json calls = Json::array();
    for (const auto& call : message.tool_calls)
        calls.push_back({{"id",call.id},{"name",call.name},{"arguments",call.arguments}});
    return {{"role",roles.at(static_cast<std::size_t>(message.role))},{"content",message.content},
        {"toolCalls",std::move(calls)},{"toolCallId",message.tool_call_id}};
}

// 原样保留送入模型的消息、工具定义与输出容量。
Json encode_request(const dfy::model::Request& input) {
    Json messages = Json::array(), tools = Json::array();
    for (const auto& message : input.messages) messages.push_back(encode_message(message));
    for (const auto& tool : input.tools)
        tools.push_back({{"name",tool.name},{"description",tool.description},{"parameters",tool.parameters}});
    return {{"messages",std::move(messages)},{"tools",std::move(tools)},
        {"maxOutputTokens",input.max_output_tokens ? Json(*input.max_output_tokens) : Json(nullptr)}};
}

// 保存模型正文和用量，原始响应与后续有效响应由事件类型区分。
Json encode_response(const dfy::model::Response& output) {
    return {{"message",encode_message(output.message)},{"finish",finishes.at(static_cast<std::size_t>(output.finish))},
        {"usage",output.usage ? Json{{"inputTokens",output.usage->input_tokens},{"outputTokens",output.usage->output_tokens}} : Json(nullptr)}};
}

// 依次尝试新目录，同一夹具可保留多轮证据。
std::filesystem::path log_path(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    for (unsigned number = 1; number <= 4096; ++number) {
        const auto name = number == 1 ? std::string("trace") : std::string("trace-")+std::to_string(number);
        const auto dir = root / name;
        if (std::filesystem::create_directory(dir)) return dir / "events.jsonl";
    }
    throw std::runtime_error("trace: no available log directory");
}
}

// 在本机创建独立证据文件，并标记本次日志的时钟和格式。
RunLog::RunLog(const std::filesystem::path& root) : path_(log_path(root)), output_(path_,std::ios::binary) {
    if (!output_) throw std::runtime_error("trace: cannot open log");
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    append("trace.open",{{"utcMs",std::chrono::duration_cast<std::chrono::milliseconds>(now).count()}});
    if (failed_) throw std::runtime_error("trace: cannot initialize log");
}

// 已退出的共享记录者不再追加，请求异常终止时保留可见的缺尾。
RunLog::~RunLog() { append("trace.closed",Json::object()); }

// 为原 Agent 来源签发同一份日志的记录回调。
agent::RunCaptureSinks RunLog::sinks(std::string source) {
    const auto log = shared_from_this();
    return {[log, source](const auto& value) { log->model(source,value); },
        [log, source](const auto& record) { log->exit(source,record); },
        [log, source](const auto& value) { log->tool(source,value); }};
}

// 串联候选参数、已接纳调用和原退出回执。
// 步骤：写入候选身份 → 区分派发状态 → 附上实际结果或拒绝事实。
void RunLog::tool(std::string_view source, const agent::ToolCapture& value) {
    const auto& entry = value.candidate;
    const auto attempt = entry.attempt;
    const auto state = value.request ? value.request->operation()->state() : entry.result;
    Json item{{"source",source},{"session",attempt.step.run.session},{"run",attempt.step.run.sequence},
        {"step",attempt.step.sequence},{"attempt",attempt.sequence},{"candidate",entry.candidate},
        {"invocation",entry.invocation},{"accepted",entry.accepted},{"callId",value.call.id},
        {"name",value.call.name},{"arguments",value.call.arguments}};
    if (entry.tool_id) item["toolId"] = *entry.tool_id;
    if (entry.confirmation) item["confirmation"] = entry.confirmation;
    if (value.phase == agent::ToolCapturePhase::exited) {
        const auto* request = value.request;
        const auto& outcome = request ? request->outcome() : agent::ToolResult{false,entry.effect,{},entry.error};
        item["result"] = states.at(static_cast<std::size_t>(state));
        item["effect"] = effects.at(static_cast<std::size_t>(outcome.effect));
        item["succeeded"] = outcome.succeeded;
        item["output"] = outcome.output;
        item["error"] = faults.at(static_cast<std::size_t>(outcome.error));
        item["fault"] = faults.at(static_cast<std::size_t>(request ? request->fault() : entry.fault));
    }
    append(tool_phases.at(static_cast<std::size_t>(value.phase)),std::move(item));
}

// 在每条记录上固定顺序和相对时钟，失败后停止写入并报告证据缺口。
void RunLog::append(std::string_view kind, Json fields) noexcept {
    if (failed_) return;
    try {
        using namespace std::chrono;
        fields["schema"] = 1;
        fields["seq"] = ++sequence_;
        fields["kind"] = kind;
        fields["timeNs"] = duration_cast<nanoseconds>(steady_clock::now()-origin_).count();
        output_ << fields.dump() << '\n' << std::flush;
        if (output_) return;
    } catch (...) {}
    failed_ = true;
    std::cerr << "Trace writing stopped; evidence is incomplete.\n";
}

// 保留每次模型请求及其真正退出，不以准备状态推断服务已调用。
// 步骤：固定运行与步骤身份 → 加入实际输入或输出 → 记录调用时刻与结果。
void RunLog::model(std::string_view source, const agent::ModelCapture& value) {
    using namespace std::chrono;
    const auto attempt = value.attempt;
    Json item{{"source",source},{"session",attempt.step.run.session},{"run",attempt.step.run.sequence},
        {"step",attempt.step.sequence},{"attempt",attempt.sequence},{"provider",value.provider},{"model",value.model_name}};
    if (value.input) item["input"] = encode_request(*value.input);
    if (value.output) item["output"] = encode_response(*value.output);
    if (value.call) {
        const auto& call = *value.call;
        item["request"] = call.request;
        item["submittedNs"] = duration_cast<nanoseconds>(call.submitted-origin_).count();
        if (call.started) item["startedNs"] = duration_cast<nanoseconds>(*call.started-origin_).count();
        if (call.finished) item["finishedNs"] = duration_cast<nanoseconds>(*call.finished-origin_).count();
        item["callOutcome"] = outcomes.at(static_cast<std::size_t>(call.outcome));
    }
    item["result"] = states.at(static_cast<std::size_t>(value.result));
    append(phases.at(static_cast<std::size_t>(value.phase)),std::move(item));
}

// 保留运行的内部终态及实际完成的正文，沿用既有诊断编码。
void RunLog::exit(std::string_view source, const agent::TurnRequest& request) {
    const auto id = request.run_id();
    const auto state = request.operation()->state();
    Json item{{"source",source},{"session",id.session},{"run",id.sequence},{"result",states.at(static_cast<std::size_t>(state))}};
    if (state == runtime::OperationState::succeeded) item["answer"] = request.answer();
    try { item["diagnostic"] = Json::parse(agent::encode_diagnostic(agent::diagnostic_snapshot(request,"trace"))); }
    catch (...) { item["diagnosticMissing"] = true; }
    append("run.exited",std::move(item));
}
}
