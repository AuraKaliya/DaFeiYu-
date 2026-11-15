#include <dafeiyu/tool_schema.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <set>
#include <stdexcept>

namespace dfy::agent {
namespace {
using Json = nlohmann::json;
// 限制解析输入大小并将损坏 JSON 转换为不带正文的契约错误。
Json parse(const std::string& source) {
    if (source.size() > 64*1024) throw std::length_error("tool schema: JSON exceeds 64 KiB");
    auto result = Json::parse(source,nullptr,false);
    if (result.is_discarded()) throw std::invalid_argument("tool schema: invalid JSON");
    return result;
}
// 判断参数是否满足明确支持的 JSON 类型，整数不接受浮点近似。
bool matches(const std::string& type, const Json& value) {
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    if (type == "string") return value.is_string();
    if (type == "boolean") return value.is_boolean();
    if (type == "integer") return value.is_number_integer();
    if (type == "number") return value.is_number();
    return false;
}
// 校验有限 schema 子集，拒绝远程引用和任何未实现关键字。
// 步骤：检查深度与类型 → 校验本类关键字 → 递归属性或数组元素 → 校验枚举。
void definition(const Json& schema, std::size_t depth) {
    if (depth > 8 || !schema.is_object() || !schema.contains("type") || !schema["type"].is_string())
        throw std::invalid_argument("tool schema: invalid type or depth");
    const auto type = schema["type"].get<std::string>();
    if (type != "object" && type != "array" && type != "string" && type != "integer" && type != "number" && type != "boolean")
        throw std::invalid_argument("tool schema: unsupported type");
    for (const auto& [key,value] : schema.items()) {
        const bool common = key == "type" || key == "description" || key == "enum";
        if (!common && !(type == "object" && (key == "properties" || key == "required" || key == "additionalProperties")) &&
            !(type == "array" && key == "items")) throw std::invalid_argument("tool schema: unsupported keyword");
        if (key == "description" && (!value.is_string() || value.get_ref<const std::string&>().size() > 1024))
            throw std::invalid_argument("tool schema: invalid description");
    }
    if (type == "object") {
        if (!schema.contains("properties") || !schema["properties"].is_object() || schema["properties"].size() > 32 ||
            !schema.contains("additionalProperties") || schema["additionalProperties"] != false)
            throw std::invalid_argument("tool schema: closed object properties required");
        for (const auto& [name,child] : schema["properties"].items()) {
            if (name.empty() || name.size() > 64) throw std::invalid_argument("tool schema: invalid property name");
            definition(child,depth+1);
        }
        if (schema.contains("required")) {
            const auto& required = schema["required"]; std::set<std::string> seen;
            if (!required.is_array() || required.size() > 32) throw std::invalid_argument("tool schema: invalid required list");
            for (const auto& name : required) if (!name.is_string() || !schema["properties"].contains(name.get<std::string>()) ||
                !seen.insert(name.get<std::string>()).second) throw std::invalid_argument("tool schema: unknown or duplicate required field");
        }
    } else if (type == "array") {
        if (!schema.contains("items")) throw std::invalid_argument("tool schema: array items required");
        definition(schema["items"],depth+1);
    }
    if (schema.contains("enum")) {
        const auto& values = schema["enum"];
        if (!values.is_array() || values.empty() || values.size() > 32 || type == "object" || type == "array")
            throw std::invalid_argument("tool schema: invalid scalar enumeration");
        std::set<Json> seen;
        for (const auto& value : values) if (!matches(type,value) || !seen.insert(value).second)
            throw std::invalid_argument("tool schema: invalid enumeration value");
    }
}
// 依据已校验 schema 验证参数，不执行代码、转换字符串类型或补默认值。
// 步骤：检查类型和枚举 → 校验对象必需及额外字段 → 递归子值或有界数组。
void arguments(const Json& schema, const Json& value) {
    const auto type = schema["type"].get<std::string>();
    if (!matches(type,value)) throw std::invalid_argument("tool arguments: type mismatch");
    if (schema.contains("enum") && std::find(schema["enum"].begin(),schema["enum"].end(),value) == schema["enum"].end())
        throw std::invalid_argument("tool arguments: value outside enumeration");
    if (type == "object") {
        if (schema.contains("required")) for (const auto& key : schema["required"]) if (!value.contains(key.get<std::string>()))
            throw std::invalid_argument("tool arguments: missing required field");
        for (const auto& [name,child] : value.items()) {
            if (!schema["properties"].contains(name)) throw std::invalid_argument("tool arguments: extra field");
            arguments(schema["properties"][name],child);
        }
    } else if (type == "array") {
        if (value.size() > 64) throw std::length_error("tool arguments: array exceeds 64 items");
        for (const auto& child : value) arguments(schema["items"],child);
    }
}
}
struct ToolSchema::Storage { std::string source; Json definition; };
// 准备不可变参数契约，所有结构校验成功后才发布共享存储。
// 步骤：解析有界 JSON → 校验根与递归定义 → 保存文本及解析结果。
ToolSchema::ToolSchema(std::string json) {
    auto schema = parse(json); definition(schema,1);
    if (schema["type"] != "object") throw std::invalid_argument("tool schema: object root required");
    storage_ = std::make_shared<const Storage>(std::move(json),std::move(schema));
}
// 校验本次有界参数，实例共享的定义始终保持不变。
void ToolSchema::validate(const std::string& input) const { arguments(storage_->definition,parse(input)); }
// 返回契约原文供模型描述投影，不暴露内部 JSON 库类型。
const std::string& ToolSchema::json() const noexcept { return storage_->source; }
}
