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
    std::optional<std::string> reasoning_content;
    // 由宿主在工具结果提交时填写，仅影响本地投影，不进入供应商协议。
    std::uint32_t receipt_view_bytes = 0;
    bool settled_evidence = false;
    // 仅宿主最终提交的普通问答记录其原始消息数，供应商和模型无法指定该标记。
    std::uint8_t completed_dialogue_span = 0;
};
// 参数描述保留为 JSON 文本，供应商与 JSON 库类型不进入 Agent 公共接口。
struct ToolDefinition { std::string name, description, parameters; };
struct Request {
    std::vector<Message> messages;
    std::vector<ToolDefinition> tools;
    std::optional<std::uint32_t> max_output_tokens;
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
// 校验统一请求的角色、工具配对、定义和容量，失败时不接触凭据或网络。
void validate_request(const Request& request);
// 校验完整模型响应及工具参数，截断调用和不一致结束原因均明确拒绝。
void validate_response(const Response& response);
}
