#include <dafeiyu/model_protocol.hpp>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace dfy::model {
namespace {
using Json = nlohmann::json;
// 将已校验角色转换为两家兼容接口使用的字段值。
const char* role_name(Role role) {
    switch (role) {
    case Role::user: return "user";
    case Role::assistant: return "assistant";
    case Role::system: return "system";
    case Role::tool: return "tool";
    }
    throw std::invalid_argument("model: unsupported role");
}
// 保留服务端停止原因，未知值拒绝而不默认视为正常结束。
FinishReason finish_reason(const std::string& reason) {
    if (reason == "stop") return FinishReason::stop;
    if (reason == "tool_calls") return FinishReason::tool_calls;
    if (reason == "length") return FinishReason::length;
    if (reason == "content_filter") return FinishReason::content_filter;
    throw std::runtime_error("model: unsupported finish reason");
}
}
// 编码带工具历史的统一请求，供应商特有字段只在模型层选择。
// 步骤：校验统一请求 → 序列化消息和定义 → 选择供应商参数 → 输出 UTF-8 JSON。
std::string encode_request(const Config& config, const Request& request) {
    validate_request(request);
    try {
        auto messages = Json::array();
        for (const auto& message : request.messages) {
            Json item{{"role",role_name(message.role)},{"content",message.content}};
            if (!message.tool_calls.empty()) {
                item["tool_calls"] = Json::array();
                for (const auto& call : message.tool_calls) item["tool_calls"].push_back({{"id",call.id},{"type","function"},
                    {"function",{{"name",call.name},{"arguments",call.arguments}}}});
            }
            if (message.role == Role::tool) item["tool_call_id"] = message.tool_call_id;
            messages.push_back(std::move(item));
        }
        Json body{{"model",config.model},{"stream",false},{"messages",std::move(messages)}};
        if (!request.tools.empty()) {
            body["tools"] = Json::array();
            for (const auto& tool : request.tools) body["tools"].push_back({{"type","function"},
                {"function",{{"name",tool.name},{"description",tool.description},{"parameters",Json::parse(tool.parameters)}}}});
        }
        if (request.max_output_tokens) body["max_tokens"] = *request.max_output_tokens;
        if (config.provider == "deepseek") body["thinking"] = {{"type","disabled"}};
        else if (config.provider == "qwen") body["enable_thinking"] = false;
        else throw std::invalid_argument("provider: expected deepseek or qwen");
        return body.dump();
    } catch (const Json::exception&) { throw std::invalid_argument("model: invalid UTF-8 or JSON request"); }
}
// 解码完整响应，参数只作为数据交付，任何截断调用都在交付前拒绝。
// 步骤：检查原始容量并解析 → 提取文本、调用与停止原因 → 读取可选用量 → 校验统一响应。
Response decode_response(const std::string& body) {
    if (body.size() > 8*1024*1024) throw std::length_error("model: response exceeds 8 MiB");
    try {
        const auto document = Json::parse(body);
        const auto& choice = document.at("choices").at(0);
        const auto& message = choice.at("message");
        if (message.at("role") != "assistant") throw std::runtime_error("model: response role is not assistant");
        Response result; result.finish = finish_reason(choice.at("finish_reason").get<std::string>());
        if (message.contains("content") && !message.at("content").is_null()) result.message.content = message.at("content").get<std::string>();
        if (message.contains("tool_calls") && !message.at("tool_calls").is_null()) {
            const auto& calls = message.at("tool_calls");
            if (!calls.is_array() || calls.size() > 64) throw std::runtime_error("model: invalid tool call list");
            for (const auto& call : calls) {
                if (call.at("type") != "function") throw std::runtime_error("model: unsupported tool call type");
                const auto& function = call.at("function");
                result.message.tool_calls.push_back({call.at("id").get<std::string>(),function.at("name").get<std::string>(),
                    function.at("arguments").get<std::string>()});
            }
        }
        if (document.contains("usage") && !document.at("usage").is_null()) {
            const auto& usage = document.at("usage");
            if (!usage.at("prompt_tokens").is_number_unsigned() || !usage.at("completion_tokens").is_number_unsigned())
                throw std::runtime_error("model: invalid token usage");
            result.usage = Usage{usage.at("prompt_tokens").get<std::uint64_t>(),usage.at("completion_tokens").get<std::uint64_t>()};
        }
        validate_response(result); return result;
    } catch (const Json::exception&) { throw std::runtime_error("model: invalid structured response"); }
}
}
