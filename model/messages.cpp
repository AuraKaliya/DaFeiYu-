#include <dafeiyu/model_messages.hpp>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

namespace dfy::model {
namespace {
constexpr std::size_t max_bytes = 1024*1024;
// 限制协议名称为两家适配器共同支持的 ASCII 函数名。
bool tool_name(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    for (const auto c : name) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}
// 累加有界正文，不让整数溢出绕过请求预算。
void account(std::size_t& bytes, const std::string& text) {
    if (text.size() > max_bytes-bytes) throw std::length_error("model: request exceeds 1 MiB");
    bytes += text.size();
}
// 校验参数或 schema 为完整 JSON 对象，不将不完整参数送入执行层。
void object_json(const std::string& text) {
    const auto value = nlohmann::json::parse(text,nullptr,false);
    if (!value.is_object()) throw std::invalid_argument("model: expected JSON object");
}
}
// 在模型提交前验证完整工具对话，保持调用和结果一一配对。
// 步骤：校验容量和工具定义 → 按角色检查有序消息 → 关联调用结果 → 拒绝未闭合调用。
void validate_request(const Request& request) {
    if (request.messages.empty() || request.messages.size() > 1024 || request.tools.size() > 64 ||
        (request.max_output_tokens && !*request.max_output_tokens))
        throw std::invalid_argument("model: invalid request limits");
    std::size_t bytes = 0;
    std::set<std::string> names, calls, pending;
    for (const auto& tool : request.tools) {
        account(bytes,tool.name); account(bytes,tool.description); account(bytes,tool.parameters);
        if (!tool_name(tool.name) || !names.insert(tool.name).second || tool.parameters.size() > 64*1024)
            throw std::invalid_argument("model: invalid or duplicate tool definition");
        object_json(tool.parameters);
    }

    for (std::size_t i = 0; i < request.messages.size(); ++i) {
        const auto& message = request.messages[i];
        account(bytes,message.content); account(bytes,message.tool_call_id);
        if (message.role != Role::user && message.role != Role::assistant && message.role != Role::system && message.role != Role::tool)
            throw std::invalid_argument("model: unsupported role");
        if ((message.role == Role::system && i != 0) || (message.role != Role::tool && !pending.empty()))
            throw std::invalid_argument("model: invalid message ordering");
        if ((message.content.empty() && message.tool_calls.empty()) ||
            (message.role == Role::user && message.content.find_first_not_of(" \t\r\n") == std::string::npos))
            throw std::invalid_argument("model: nonempty message required");
        if ((message.role != Role::assistant && !message.tool_calls.empty()) || message.tool_calls.size() > 64 ||
            (message.role != Role::tool && !message.tool_call_id.empty()))
            throw std::invalid_argument("model: misplaced tool fields");

        if (message.role == Role::tool) {
            if (!pending.erase(message.tool_call_id)) throw std::invalid_argument("model: unmatched tool result");
        }
        for (const auto& call : message.tool_calls) {
            account(bytes,call.id); account(bytes,call.name); account(bytes,call.arguments);
            if (call.id.empty() || call.id.size() > 128 || !tool_name(call.name) || !calls.insert(call.id).second)
                throw std::invalid_argument("model: invalid or duplicate tool call");
            object_json(call.arguments); pending.insert(call.id);
        }
    }
    if (!pending.empty()) throw std::invalid_argument("model: missing tool results");
}
}
