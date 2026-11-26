#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dfy::model {
enum class Role { user, assistant, system, tool };
// 来源由消息的实际生产入口签发，未知来源沿用历史协议。
enum class MessageSource { unknown, human, host_status, observation, plugin, mailbox, host_plan, host_control, host_summary, model, tool, rules };
// 返回可用于离线轨迹和供应商数据封套的稳定来源名。
std::string_view source_name(MessageSource source);
struct ToolCall { std::string id, name, arguments; };
struct Message {
    Role role = Role::user;
    std::string content;
    std::vector<ToolCall> tool_calls;
    std::string tool_call_id;
    std::optional<std::string> reasoning_content;
    // 由宿主在工具结果提交时填写，仅影响本地投影，不进入供应商协议。
    std::uint32_t receipt_view_bytes = 0;
    bool settled_evidence = false;
    // 仅宿主最终提交的普通问答记录其原始消息数，供应商和模型无法指定该标记。
    std::uint8_t completed_dialogue_span = 0;
    MessageSource source = MessageSource::unknown;
};
// 按来源建立原生协议角色，保留调用者给出的完整正文。
Message sourced_message(MessageSource source, std::string content);
// 将有来源的统一消息映射成供应商可读正文，原消息保持可查。
std::string wire_content(const Message& message);
// 参数描述保留为 JSON 文本，供应商与 JSON 库类型不进入 Agent 公共接口。
struct ToolDefinition { std::string name, description, parameters; };
struct Request {
    std::vector<Message> messages;
    std::vector<ToolDefinition> tools;
    std::optional<std::uint32_t> max_output_tokens;
    // 只有启用思考且本次提供工具时，协议才随助手消息发送原推理续行。
    bool sends_reasoning(bool thinking) const noexcept { return thinking && !tools.empty(); }
};
enum class FinishReason { stop, tool_calls, length, content_filter };
struct Usage {
    std::uint64_t input_tokens = 0, output_tokens = 0;
    std::optional<std::uint64_t> cache_hit_tokens, cache_miss_tokens;
};
struct Response {
    Message message{Role::assistant,{}};
    FinishReason finish = FinishReason::stop;
    std::optional<Usage> usage;
};
// 校验角色、工具配对和定义，按本次协议计入推理续行，另行限制未发送的原文。
void validate_request(const Request& request, bool thinking = true);
// 校验完整模型响应及工具参数，截断调用和不一致结束原因均明确拒绝。
void validate_response(const Response& response);
}
