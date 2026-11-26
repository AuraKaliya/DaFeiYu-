#include "run_log.hpp"
#include "trace_model.hpp"
#include <dafeiyu/model_protocol.hpp>
#include <dafeiyu/digest.hpp>
#include <dafeiyu/diagnostic_json.hpp>
#include <dafeiyu/plugin_agents.hpp>
#include <dafeiyu/agent_abi_v2.h>
#include <array>
#include <iostream>
#include <nlohmann/json.hpp>

namespace dfy::app {
namespace {
using namespace trace_format;
constexpr std::array outcomes{"pending","returned","threw"};
constexpr std::array phases{"model.prepared","model.submitted","model.exited","model.rewritten"};
constexpr std::array tool_phases{"tool.candidate","tool.admitted","tool.exited"};

// 对比同一截止号下的工具序列，并按会话与用途记录连续两次投影的消息前缀。
// 步骤：共享供应商字段编码 → 找出首个工具差异 → 比较消息指纹和配对身份 → 保存本次有限指纹。
Json compare_input(const agent::ModelCapture& value, std::vector<std::string>& previous) {
    auto projected = *value.input;
    projected.tools = *value.projected_tools;
    const auto before = model::encode_request_fields(*value.input,value.thinking);
    const auto after = model::encode_request_fields(projected,value.thinking);
    const auto original = Json::parse(before);
    const auto next = Json::parse(after);
    const auto actual_tools = original.value("tools",Json::array());
    const auto tools = next.value("tools",Json::array());
    std::size_t index = 0;
    while (index < actual_tools.size() && index < tools.size() && actual_tools[index] == tools[index]) ++index;
    Json result{{"cutoff",value.evidence_revision},{"purpose",value.purpose},{"originalFieldBytes",before.size()},
        {"projectedFieldBytes",after.size()},{"toolsSha256",platform::sha256(tools.dump())},{"toolCount",tools.size()}};
    if (index < actual_tools.size() || index < tools.size()) result["firstToolDifference"] = index;

    const auto& messages = next.at("messages");
    std::vector<std::string> stamps;
    stamps.reserve(messages.size());
    for (const auto& message : messages) stamps.push_back(platform::sha256(message.dump()));
    const auto common = std::mismatch(stamps.begin(),stamps.end(),previous.begin(),previous.end()).first-stamps.begin();
    result["commonMessages"] = common;
    result["headSha256"] = stamps.front();
    if (static_cast<std::size_t>(common) < messages.size()) {
        const auto& first = messages.at(common);
        result["firstChangedRole"] = first.at("role");
        if (first.contains("tool_call_id")) result["firstChangedCall"] = first.at("tool_call_id");
    }
    previous = std::move(stamps);
    return result;
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
agent::RunCaptureSinks RunLog::sinks(std::string source, bool compare_projection) {
    const auto log = shared_from_this();
    auto previous = std::make_shared<std::map<std::string,std::vector<std::string>>>();
    return {[log, source, previous](const auto& value) { log->model(source,value,*previous); },
        [log, source](const auto& record) { log->exit(source,record); },
        [log, source](const auto& value) { log->tool(source,value); },compare_projection || source == "main"};
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
        const auto& outcome = request ? request->outcome() : agent::ToolResult{false,entry.effect,entry.rejection,entry.error};
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

// 以原绑定和宿主回执记录已生效的工作修订，供会话结束后串查。
// 步骤：组装工作及作用域身份 → 附上已验正文及阶段 → 隔离诊断写入失败。
void RunLog::work_fact(runtime::DomainId owner, runtime::DomainId session, bool retired, const plugins::SubagentWorkFact& fact,
    std::uint64_t receipt) noexcept {
    try {
        const auto phase = fact.phase == DFY_V2_WORK_ACCEPTED ? "accepted" : fact.phase == DFY_V2_WORK_EXIT ? "exit" : "effect";
        append("binding.work_settled",{{"owner",owner},{"session",session},{"epoch",fact.epoch},{"work",fact.work},
            {"revision",fact.revision},{"receipt",receipt},{"phase",phase},{"finalEffect",fact.final_effect},{"retired",retired},
            {"body",Json::parse(fact.content)}});
    } catch (...) { failed_ = true; }
}

// 保存本体收到的原文本和关联身份，记录失败不影响原队列及主会话。
void RunLog::mail(std::string_view kind, runtime::DomainId mailbox, runtime::DomainId sender, events::EventId id,
    std::string_view category, std::string_view body, std::optional<agent::RunId> run) noexcept {
    try {
        Json fields{{"mailbox",mailbox},{"sender",sender},{"domain",id.domain},{"message",id.sequence},{"category",category},{"body",body}};
        if (run) {
            fields["session"] = run->session;
            fields["run"] = run->sequence;
        }
        append(kind,std::move(fields));
    } catch (...) {}
}

// 保留每次模型请求及其真正退出，不以准备状态推断服务已调用。
// 步骤：固定运行与步骤身份 → 加入实际输入或输出 → 记录调用时刻与结果。
void RunLog::model(std::string_view source, const agent::ModelCapture& value,
    std::map<std::string,std::vector<std::string>>& previous) {
    using namespace std::chrono;
    const auto attempt = value.attempt;
    Json item{{"source",source},{"session",attempt.step.run.session},{"run",attempt.step.run.sequence},
        {"step",attempt.step.sequence},{"attempt",attempt.sequence},{"provider",value.provider},{"model",value.model_name}};
    item["thinking"] = value.thinking;
    if (!value.reasoning_effort.empty()) item["reasoningEffort"] = value.reasoning_effort;
    if (!value.persona_profile.empty()) item["personaProfile"] = value.persona_profile;
    if (!value.prompt_template.empty()) item["promptTemplate"] = value.prompt_template;
    if (value.input) {
        item["input"] = encode_request(*value.input);
        item["compressedBatches"] = value.compressed_batches;
    }
    if (value.input && value.projected_tools) {
        try { item["projection"] = compare_input(value,previous[std::string(value.purpose)]); }
        catch (...) { item["projectionUnavailable"] = true; }
    }
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
    for (const auto& input : request.inputs())
        append("dialogue.input_settled",{{"source",source},{"session",id.session},{"run",id.sequence},{"message",input.message},
            {"state",agent::input_state_name(input.state)},{"modelStep",input.model_step}});
    if (state == runtime::OperationState::succeeded) item["answer"] = request.answer();
    try { item["diagnostic"] = Json::parse(agent::encode_diagnostic(agent::diagnostic_snapshot(request,"trace"))); }
    catch (...) { item["diagnosticMissing"] = true; }
    append("run.exited",std::move(item));
}
}
