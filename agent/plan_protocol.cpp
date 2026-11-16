#include <dafeiyu/plan_protocol.hpp>
#include <dafeiyu/utf8.hpp>
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
// 步骤：核对目标及原版本 → 编码原工具范围和可选旧计划 → 添加固定契约说明 → 校验完整模型请求。
PlanPrompt prepare_plan_prompt(const PlanScope& scope, std::uint64_t expected_revision, std::string_view goal, const PlanDefinition* previous) {
    const PlanBinding expected{scope.run(),scope.goal_revision(),expected_revision}; binding(scope,expected);
    if (goal.empty() || goal.size() > 64*1024 || !runtime::valid_utf8(goal) || goal.find_first_not_of(" \t\r\n") == std::string_view::npos ||
        (previous && previous->binding() != expected) || (!previous && expected_revision))
        throw std::invalid_argument("plan protocol: goal and matching previous definition required");
    Json data{{"goal",goal},{"allowed_tool_ids",scope.tools()},{"maximum_steps",scope.limits().maximum_steps},
        {"maximum_text_bytes",scope.limits().maximum_text_bytes}};
    if (previous) data["previous_steps"] = definition_data(*previous);
    const std::string instructions =
        "请提出有界顺序计划，只返回 JSON：{\"schema_version\":1,\"steps\":[{\"id\":\"step_id\",\"objective\":\"步骤目标\","
        "\"completion_criterion\":\"待宿主验证的完成条件\",\"tools\":[]}]}。工具 ID 只能来自数据中的 allowed_tool_ids；纯文本步骤使用空数组。"
        "这里只提出步骤，不执行工具，不返回状态、权限、目标修订或推理草稿。宿主依据实际工作和证据确认完成。";
    model::Request request{{{model::Role::system,instructions},{model::Role::user,data.dump()}}};
    model::validate_request(request); return {expected,std::move(request)};
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
}
