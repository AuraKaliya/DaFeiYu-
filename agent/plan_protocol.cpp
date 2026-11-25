#include <dafeiyu/plan_protocol.hpp>
#include <dafeiyu/utf8.hpp>
#include <dafeiyu/digest.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <set>
#include <stdexcept>

namespace dfy::agent {
namespace {
using Json = nlohmann::json;
// 只接受本版本定义的对象字段，模型不能夹带状态、权限或目标修订。
void fields(const Json& value, std::initializer_list<std::string_view> names) {
    if (!value.is_object() || value.size() != names.size()) throw std::invalid_argument("plan protocol: exact object fields required");
    for (const auto& item : value.items()) if (std::find(names.begin(),names.end(),item.key()) == names.end())
        throw std::invalid_argument("plan protocol: unknown field");
}
// 检查候选所属的宿主范围及有限修订，旧版本不能重新标成当前目标。
void binding(const PlanScope& scope, PlanBinding expected) {
    if (expected.run != scope.run() || expected.goal_revision != scope.goal_revision() || expected.plan_revision >= scope.limits().maximum_revisions)
        throw std::invalid_argument("plan protocol: stale scope or revision limit");
}
// 将既有定义编码为数据，不包含原工作状态、工具执行参数或完成权。
Json definition_data(const PlanDefinition& definition) {
    Json result = Json::array();
    for (const auto& step : definition.steps()) result.push_back({{"id",step.id},{"objective",step.objective},
        {"completion_criterion",step.completion_criterion},{"tools",step.tools}});
    return result;
}
// 有界解析候选，重复字段和超深结构不会采用后写覆盖的解释。
// 步骤：检查输入字节 → 逐对象跟踪字段 → 拒绝过深及重复键 → 返回候选数据。
Json parse_candidate(const std::string& text) {
    if (text.size() > 64*1024) throw std::length_error("plan protocol: candidate exceeds 64 KiB");
    std::vector<std::set<std::string>> objects;
    // 解析事件只维护有界对象字段集合，不调用工具或接受模型附带状态。
    // 步骤：核对深度 → 进入或退出对象 → 在字段事件拒绝重复名称。
    return Json::parse(text,[&](int depth,Json::parse_event_t event,Json& value) {
        if (depth > 8) throw std::invalid_argument("plan protocol: nesting limit exceeded");
        if (event == Json::parse_event_t::object_start) objects.emplace_back();
        else if (event == Json::parse_event_t::object_end) objects.pop_back();
        else if (event == Json::parse_event_t::key && !objects.back().insert(value.get<std::string>()).second)
            throw std::invalid_argument("plan protocol: duplicate field");
        return true;
    });
}
}
// 显式规划请求只索取候选数据，普通问答和工具运行不会被这个接口自动切换模式。
// 步骤：核对目标、原版本及受保护前缀 → 编码原范围和修订数据 → 添加固定契约说明 → 校验完整模型请求。
PlanPrompt prepare_plan_prompt(const PlanScope& scope, std::uint64_t expected_revision, std::string_view goal,
    const PlanDefinition* previous, std::size_t completed_prefix, std::string_view guidance) {
    const PlanBinding expected{scope.run(),scope.goal_revision(),expected_revision}; binding(scope,expected);
    if (goal.empty() || goal.size() > 64*1024 || !runtime::valid_utf8(goal) || goal.find_first_not_of(" \t\r\n") == std::string_view::npos ||
        (previous && previous->binding() != expected) || (!previous && (expected_revision || completed_prefix)) ||
        (previous && completed_prefix > previous->steps().size()) || guidance.size() > 2048 || !runtime::valid_utf8(guidance))
        throw std::invalid_argument("plan protocol: goal and matching previous definition required");
    Json data{{"goal",goal},{"allowed_tool_ids",scope.tools()},{"maximum_steps",scope.limits().maximum_steps},
        {"maximum_text_bytes",scope.limits().maximum_text_bytes}};
    if (previous) { data["previous_steps"] = definition_data(*previous); data["completed_prefix"] = completed_prefix; }
    if (!guidance.empty()) data["revision_guidance"] = guidance;
    const std::string instructions =
        "请提出有界顺序计划，只返回 JSON：{\"schema_version\":1,\"steps\":[{\"id\":\"step_id\",\"objective\":\"步骤目标\","
        "\"completion_criterion\":\"待宿主验证的完成条件\",\"tools\":[]}]}。工具 ID 只能来自数据中的 allowed_tool_ids；纯文本步骤使用空数组。"
        "这里只提出步骤，不执行工具，不返回状态、权限、目标修订或推理草稿。宿主依据实际工作和证据确认完成。"
        "若有旧计划，前 completed_prefix 项必须逐字段保留，只能修订后续步骤；修订数据不能授予新权限。";
    model::Request request{{{model::Role::system,instructions},{model::Role::user,data.dump()}}};
    model::validate_request(request); return {expected,std::move(request)};
}
// 投影原调用配对、结果及宿主已验证的前缀，较长原文仍可按消息序号回查。
// 步骤：提取最近的原工具回执 → 保留消息序号与超长引用 → 附入已验证步骤 → 校验规划请求。
void project_plan_evidence(model::Request& plan, const std::vector<model::Message>& history, const PlanSnapshot& state,
    std::string_view status) {
    if (!plan.tools.empty() || plan.messages.size() != 2 || plan.messages.back().role != model::Role::user)
        throw std::invalid_argument("plan evidence: expected a planning request");
    auto receipts = Json::array();
    std::size_t omitted = 0;
    for (std::size_t end = history.size(); end > 0;) {
        if (history[end-1].role != model::Role::tool) { --end; continue; }
        auto begin = end-1;
        while (begin && history[begin-1].role == model::Role::tool) --begin;
        if (!begin) throw std::logic_error("plan evidence: missing tool batch");
        const auto& calls = history[begin-1].tool_calls;
        for (auto i = end; i-- > begin;) {
            const auto& message = history[i];
            const bool recent = receipts.size() < 12;
            if (!recent && message.settled_evidence) { ++omitted; continue; }
            const auto found = std::ranges::find(calls,message.tool_call_id,&model::ToolCall::id);
            if (found == calls.end()) throw std::logic_error("plan evidence: unmatched tool result");
            Json fact{{"call_id",found->id},{"tool",found->name},{"message",i+1},{"batch_message",begin}};
            const auto preview = recent ? std::size_t{1536} : std::size_t{256};
            if (message.content.size() <= preview+128) fact["receipt"] = message.content;
            else {
                auto bytes = preview;
                while ((static_cast<unsigned char>(message.content[bytes]) & 0xc0) == 0x80) --bytes;
                fact["receipt_prefix"] = message.content.substr(0,bytes);
                fact["original_bytes"] = message.content.size();
                fact["sha256"] = platform::sha256(message.content);
            }
            receipts.push_back(std::move(fact));
        }
        end = begin-1;
    }
    std::reverse(receipts.begin(),receipts.end());
    auto verified = Json::array();
    for (const auto& step : state.steps) {
        if (step.state == PlanStepState::completed || step.state == PlanStepState::skipped)
            verified.push_back({{"step",step.key.step},{"revision",step.key.plan.plan_revision},
                {"state",step.state == PlanStepState::completed ? "completed" : "skipped"},{"evidence",step.evidence}});
    }
    auto data = Json::parse(plan.messages.back().content);
    data["host_evidence"] = {{"through_message",history.size()},{"tool_receipts",std::move(receipts)},
        {"older_tool_receipts",omitted},{"verified_steps",std::move(verified)}};
    if (!status.empty()) data["host_evidence"]["context_status"] = Json::parse(status);
    plan.messages.back().content = data.dump();
    model::validate_request(plan);
}
// 将完整模型文本转换为原范围下的新候选，解析不执行工具或推进 PlanBook。
// 步骤：核对原绑定与响应完整性 → 严格解析格式和字段 → 复制声明式步骤 → 复用原范围与预算校验。
PlanDefinition parse_plan_response(const PlanScope& scope, PlanBinding expected, const model::Response& response) {
    binding(scope,expected);
    if (response.finish != model::FinishReason::stop || !response.message.tool_calls.empty())
        throw std::invalid_argument("plan protocol: complete proposal text required");
    if (response.message.content.size() > 64*1024) throw std::length_error("plan protocol: candidate exceeds 64 KiB");
    model::validate_response(response);
    try {
        const auto root = parse_candidate(response.message.content); fields(root,{"schema_version","steps"});
        if (!root.at("schema_version").is_number_integer() || root.at("schema_version") != 1 || !root.at("steps").is_array() ||
            root.at("steps").size() > scope.limits().maximum_steps) throw std::invalid_argument("plan protocol: unsupported schema or step count");
        std::vector<PlanStepDefinition> steps; steps.reserve(root.at("steps").size());
        for (const auto& value : root.at("steps")) {
            fields(value,{"id","objective","completion_criterion","tools"});
            if (!value.at("tools").is_array() || value.at("tools").size() > 8) throw std::invalid_argument("plan protocol: invalid tool list");
            steps.push_back({value.at("id").get<std::string>(),value.at("objective").get<std::string>(),
                value.at("completion_criterion").get<std::string>(),value.at("tools").get<std::vector<std::string>>()});
        }
        return PlanDefinition::create(scope,expected.plan_revision+1,std::move(steps));
    } catch (const Json::exception&) { throw std::invalid_argument("plan protocol: malformed candidate"); }
}
// 只投影原候选中的指定步骤，模型不能通过这个数据入口选择其他版本。
std::string plan_step_input(const PlanDefinition& definition, std::size_t index) {
    const auto& step = definition.steps().at(index); const auto bound = definition.binding();
    return Json{{"goal_revision",bound.goal_revision},{"plan_revision",bound.plan_revision},{"step_id",step.id},
        {"objective",step.objective},{"completion_criterion",step.completion_criterion},{"allowed_tool_ids",step.tools}}.dump();
}
}
