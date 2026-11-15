#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dfy::model {
enum class Role { user, assistant, system, tool };
struct ToolCall { std::string id, name, arguments; };
struct Message {
    Role role = Role::user;
    std::string content;
    std::vector<ToolCall> tool_calls;
    std::string tool_call_id;
};
// 参数描述保留为 JSON 文本，供应商与 JSON 库类型不进入 Agent 公共接口。
struct ToolDefinition { std::string name, description, parameters; };
struct Request {
    std::vector<Message> messages;
    std::vector<ToolDefinition> tools;
    std::optional<std::uint32_t> max_output_tokens;
};
enum class FinishReason { stop, tool_calls, length, content_filter };
struct Usage { std::uint64_t input_tokens = 0, output_tokens = 0; };
struct Response {
    Message message{Role::assistant,{}};
    FinishReason finish = FinishReason::stop;
    std::optional<Usage> usage;
};
// 校验统一请求的角色、工具配对、定义和容量，失败时不接触凭据或网络。
void validate_request(const Request& request);
// 校验完整模型响应及工具参数，截断调用和不一致结束原因均明确拒绝。
void validate_response(const Response& response);
}
