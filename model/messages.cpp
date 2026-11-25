#include <dafeiyu/model_messages.hpp>
#include <dafeiyu/utf8.hpp>
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
// 校验 UTF-8 并累加有界正文，不让非法编码或整数溢出绕过请求边界。
void account(std::size_t& bytes, const std::string& text, std::size_t limit = max_bytes) {
    if (text.size() > limit-bytes) throw std::length_error("model: message payload exceeds limit");
    if (!runtime::valid_utf8(text)) throw std::invalid_argument("model: invalid UTF-8 text");
    bytes += text.size();
}
// 校验参数或 schema 为完整 JSON 对象，不将不完整参数送入执行层。
void object_json(const std::string& text) {
    const auto value = nlohmann::json::parse(text,nullptr,false);
    if (!value.is_object()) throw std::invalid_argument("model: expected JSON object");
}
}
// 在模型提交前验证工具对话，发送字段与仅本地留存的推理原文分别核算容量。
// 步骤：校验限额和工具定义 → 按角色与协议计入消息 → 关联调用结果 → 拒绝未闭合调用。
void validate_request(const Request& request, bool thinking) {
    if (request.messages.empty() || request.messages.size() > 1024 || request.tools.size() > 64 ||
        (request.max_output_tokens && !*request.max_output_tokens))
        throw std::invalid_argument("model: invalid request limits");
    std::size_t bytes = 0;
    std::size_t omitted_reasoning_bytes = 0;
    const bool sends_reasoning = request.sends_reasoning(thinking);
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
        if (message.reasoning_content) {
            if (message.role != Role::assistant) throw std::invalid_argument("model: misplaced reasoning");
            if (sends_reasoning) account(bytes,*message.reasoning_content);
            else account(omitted_reasoning_bytes,*message.reasoning_content,8*max_bytes);
        }
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
        // 已闭合历史批次中的同名 ID 不属于当前调用，配对由消息顺序确定。
        if (message.role == Role::assistant) calls.clear();
        for (const auto& call : message.tool_calls) {
            account(bytes,call.id); account(bytes,call.name); account(bytes,call.arguments);
            if (call.id.empty() || call.id.size() > 128 || !tool_name(call.name) || !calls.insert(call.id).second)
                throw std::invalid_argument("model: invalid or duplicate tool call");
            object_json(call.arguments); pending.insert(call.id);
        }
    }
    if (!pending.empty()) throw std::invalid_argument("model: missing tool results");
}
// 对完整文本和工具调用使用同一响应边界，不把长度截断作为可执行参数。
// 步骤：检查角色和结束原因 → 校验文本与调用容量 → 检查调用身份和完整参数。
void validate_response(const Response& response) {
    const auto& message = response.message;
    if (message.role != Role::assistant || !message.tool_call_id.empty() || message.tool_calls.size() > 64)
        throw std::invalid_argument("model: invalid assistant response");
    if (response.finish != FinishReason::stop && response.finish != FinishReason::tool_calls &&
        response.finish != FinishReason::length && response.finish != FinishReason::content_filter)
        throw std::invalid_argument("model: unknown finish reason");
    if ((response.finish == FinishReason::tool_calls) != !message.tool_calls.empty())
        throw std::invalid_argument("model: incomplete or inconsistent tool calls");
    if (response.finish == FinishReason::stop && message.content.empty())
        throw std::invalid_argument("model: empty final text");

    std::size_t bytes = 0; account(bytes,message.content,8*max_bytes);
    if (message.reasoning_content) account(bytes,*message.reasoning_content,8*max_bytes);
    std::set<std::string> calls;
    for (const auto& call : message.tool_calls) {
        account(bytes,call.id,8*max_bytes); account(bytes,call.name,8*max_bytes); account(bytes,call.arguments,8*max_bytes);
        if (call.id.empty() || call.id.size() > 128 || !tool_name(call.name) || !calls.insert(call.id).second)
            throw std::invalid_argument("model: invalid or duplicate response tool call");
        object_json(call.arguments);
    }
}
}
