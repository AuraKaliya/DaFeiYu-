#pragma once
#include "scenario_replay.hpp"
#include <nlohmann/json.hpp>
#include <fstream>

namespace dfy::tests {
namespace material {
using Json = nlohmann::json;
// 只接受已定义的对象字段，路径、插件和服务入口不能被静默忽略。
inline void keys(const Json& value, std::initializer_list<std::string_view> allowed) {
    if (!value.is_object()) throw std::invalid_argument("material: object required");
    for (const auto& item : value.items()) if (std::find(allowed.begin(),allowed.end(),item.key()) == allowed.end())
        throw std::invalid_argument("material: unknown field");
}
// 读取有界非负整数，拒绝浮点、布尔及整数截断。
inline std::uint64_t number(const Json& value, std::uint64_t maximum) {
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 0)) throw std::invalid_argument("material: integer required");
    const auto result = value.get<std::uint64_t>(); if (result > maximum) throw std::invalid_argument("material: integer out of range"); return result;
}
// 只读枚举的明确文本名称，未知值不映射为默认执行行为。
template<class E> E named(const Json& value, std::initializer_list<std::pair<std::string_view,E>> choices) {
    const auto text = value.get<std::string>(); for (const auto& [name,result] : choices) if (text == name) return result;
    throw std::invalid_argument("material: unknown enum");
}
// 限制数组数量，防止解析后产生无界工具或控制记录。
inline const Json& array(const Json& value, std::size_t maximum) {
    if (!value.is_array() || value.size() > maximum) throw std::invalid_argument("material: bounded array required"); return value;
}
// 读取固定效果名称，不推断工具是否已经执行。
inline agent::ToolEffect effect(const Json& value) {
    using E = agent::ToolEffect; return named<E>(value,{{"not_applied",E::not_applied},{"applied",E::applied},{"unknown",E::unknown}});
}
// 在字节和嵌套限制内解析 JSON，重复键明确拒绝而非采用最后一个值。
// 步骤：限制输入字节 → 按对象维护字段集合 → 拒绝超深或重复键 → 交付纯数据。
inline Json parse(std::string_view text) {
    if (text.size() > 2*1024*1024) throw std::length_error("material: document exceeds 2 MiB");
    std::vector<std::set<std::string>> objects;
    // 在解析回调中维护当前对象的字段集合，数组不会改变对象所有权层次。
    // 步骤：检查深度 → 按对象进入／退出调整集合 → 在字段事件拒绝重复。
    return Json::parse(text,[&](int depth,Json::parse_event_t event,Json& value) {
        if (depth > 20) throw std::invalid_argument("material: nesting limit exceeded");
        if (event == Json::parse_event_t::object_start) objects.emplace_back();
        else if (event == Json::parse_event_t::object_end) objects.pop_back();
        else if (event == Json::parse_event_t::key && !objects.back().insert(value.get<std::string>()).second)
            throw std::invalid_argument("material: duplicate field");
        return true;
    });
}
}
// 从受限数据构造固定场景；解析只生成值和校验，不创建 Session 或执行工具。
// 步骤：检查格式及预算 → 读取固定夹具和模型序列 → 读取控制及预期 → 复用现有契约验证。
inline ReplayCase parse_replay_value(const material::Json& root) {
    using namespace agent; using namespace model; using namespace runtime; using namespace material;
    keys(root,{"schema_version","id","version","prompt","timeout_ms","policy","fixtures","steps","decisions","expected"});
    ReplayCase result; result.model.schema_version = static_cast<std::uint32_t>(number(root.at("schema_version"),1));
    result.model.id = root.at("id").get<std::string>(); result.model.version = static_cast<std::uint32_t>(number(root.at("version"),UINT32_MAX));
    result.prompt = root.at("prompt").get<std::string>(); result.timeout = std::chrono::milliseconds(number(root.at("timeout_ms"),3600000));
    if (result.prompt.empty() || result.prompt.size() > 1024*1024 || result.timeout.count() == 0) throw std::invalid_argument("material: prompt and timeout required");
    const auto& policy = root.at("policy"); keys(policy,{"revision","model_limit","tool_limit","allow_observation","confirm_changes"});
    result.policy = {number(policy.at("revision"),UINT64_MAX),static_cast<std::size_t>(number(policy.at("model_limit"),64)),
        static_cast<std::size_t>(number(policy.at("tool_limit"),64)),policy.at("allow_observation").get<bool>(),policy.at("confirm_changes").get<bool>()};
    std::size_t output_bytes = 0; ToolCatalog validation;
    for (const auto& item : array(root.at("fixtures"),64)) {
        keys(item,{"id","name","description","parameters","changes_state","idempotent","results"});
        ReplayTool tool; tool.description = {item.at("id").get<std::string>(),result.tools.size()+1,1,
            {item.at("name").get<std::string>(),item.at("description").get<std::string>(),item.at("parameters").dump()},
            item.at("changes_state").get<bool>(),item.at("idempotent").get<bool>()};
        if (!tool.description.id.starts_with("fixture/")) throw std::invalid_argument("material: fixed fixture namespace required");
        for (const auto& output : array(item.at("results"),64)) {
            keys(output,{"succeeded","effect","output","error"}); using E = ToolError;
            const auto error = named<E>(output.at("error"),{{"none",E::none},{"failed",E::failed},{"unavailable",E::unavailable},
                {"invalid_result",E::invalid_result},{"cancelled",E::cancelled},{"deadline",E::deadline}});
            auto content = output.at("output").get<std::string>(); output_bytes += content.size();
            if (content.size() > 128*1024 || output_bytes > 8*1024*1024) throw std::length_error("material: fixed results exceed limits");
            tool.results.push_back({output.at("succeeded").get<bool>(),effect(output.at("effect")),std::move(content),error});
        }
        validation.add(tool.description); result.model.fixtures.push_back(tool.description.id); result.tools.push_back(std::move(tool));
    }
    for (const auto& item : array(root.at("steps"),64)) {
        keys(item,{"last_role","last_text","last_tool_id","tools","response","fail","advance_ms"}); ScriptStep step;
        step.last_role = named<Role>(item.at("last_role"),{{"user",Role::user},{"assistant",Role::assistant},{"tool",Role::tool},{"system",Role::system}});
        if (item.contains("last_text")) step.last_text = item.at("last_text").get<std::string>();
        if (item.contains("last_tool_id")) step.last_tool_id = item.at("last_tool_id").get<std::string>();
        step.tool_names = array(item.at("tools"),64).get<std::vector<std::string>>();
        if (item.contains("fail")) step.fail = item.at("fail").get<bool>();
        if (item.contains("advance_ms")) step.advance_by = std::chrono::milliseconds(number(item.at("advance_ms"),3600000));
        if (item.contains("response")) {
            const auto& value = item.at("response"); keys(value,{"text","finish","tool_calls"}); Response response;
            response.message.content = value.at("text").get<std::string>();
            response.finish = named<FinishReason>(value.at("finish"),{{"stop",FinishReason::stop},{"tool_calls",FinishReason::tool_calls},
                {"length",FinishReason::length},{"content_filter",FinishReason::content_filter}});
            if (value.contains("tool_calls")) for (const auto& call : array(value.at("tool_calls"),64)) {
                keys(call,{"id","name","arguments"}); if (!call.at("arguments").is_object()) throw std::invalid_argument("material: object arguments required");
                response.message.tool_calls.push_back({call.at("id").get<std::string>(),call.at("name").get<std::string>(),call.at("arguments").dump()});
            }
            step.response = std::move(response);
        }
        result.model.steps.push_back(std::move(step));
    }
    for (const auto& choice : array(root.at("decisions"),64)) result.decisions.push_back(named<ReplayChoice>(choice,
        {{"approve",ReplayChoice::approve},{"deny",ReplayChoice::deny},{"cancel",ReplayChoice::cancel},{"expire",ReplayChoice::expire}}));
    const auto& expected = root.at("expected"); keys(expected,{"model_requests","tool_calls","history_messages","result","stop_reason","effects"});
    result.model.expected_model_requests = static_cast<std::size_t>(number(expected.at("model_requests"),64));
    result.expected.tool_calls = static_cast<std::size_t>(number(expected.at("tool_calls"),64));
    result.expected.history_messages = static_cast<std::size_t>(number(expected.at("history_messages"),1024));
    result.expected.result = named<OperationState>(expected.at("result"),{{"succeeded",OperationState::succeeded},{"failed",OperationState::failed},{"cancelled",OperationState::cancelled}});
    using P = PolicyReason; result.expected.stop_reason = named<P>(expected.at("stop_reason"),{{"none",P::none},{"cancelled",P::cancelled},
        {"deadline",P::deadline},{"model_limit",P::model_limit},{"tool_limit",P::tool_limit},{"no_progress",P::no_progress},{"policy_failed",P::policy_failed}});
    for (const auto& value : array(expected.at("effects"),64)) result.expected.effects.push_back(effect(value));
    ExecutionPolicy({1,1},1,result.policy); ScriptedModel verify(result.model,result.model.fixtures); return result;
}
// 严格解析原文本后校验场景契约。
inline ReplayCase parse_replay_material(std::string_view text) { return parse_replay_value(material::parse(text)); }

// 只读取宿主明确选择的有界场景文件，文件内容不能选择执行入口。
// 步骤：打开指定文件 → 最多读取上限加一个字节 → 检查读取错误 → 解析固定场景值。
inline ReplayCase load_replay_material(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary); if (!file) throw std::runtime_error("material: cannot open fixture");
    std::string text(2*1024*1024+1,'\0'); file.read(text.data(),static_cast<std::streamsize>(text.size()));
    if (file.bad()) throw std::runtime_error("material: fixture read failed");
    text.resize(static_cast<std::size_t>(file.gcount())); return parse_replay_material(text);
}
}
