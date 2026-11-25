#pragma once
#include <array>
#include <dafeiyu/model_messages.hpp>
#include <dafeiyu/tool_work.hpp>
#include <nlohmann/json.hpp>

namespace dfy::app::trace_format {
using Json = nlohmann::json;
inline constexpr std::array roles{"user","assistant","system","tool"};
inline constexpr std::array finishes{"stop","tool_calls","length","content_filter"};
inline constexpr std::array states{"pending","succeeded","failed","cancelled"};
inline constexpr std::array effects{"not_applied","applied","unknown"};
inline constexpr std::array faults{"none","failed","unavailable","invalid_result","cancel_failed","read_failed","release_failed","cancelled","deadline"};

// 保留统一模型协议中的工具配对及可选供应商续行内容。
// 步骤：序列化有序工具请求 → 保存消息正文和原配对身份 → 附上可选续行内容。
inline Json encode_message(const model::Message& message) {
    Json calls = Json::array();
    for (const auto& call : message.tool_calls)
        calls.push_back({{"id",call.id},{"name",call.name},{"arguments",call.arguments}});
    Json item{{"role",roles.at(static_cast<std::size_t>(message.role))},{"content",message.content},
        {"toolCalls",std::move(calls)},{"toolCallId",message.tool_call_id}};
    if (message.reasoning_content) item["reasoningContent"] = *message.reasoning_content;
    return item;
}

// 原样保留送入模型的消息、工具定义与输出容量。
inline Json encode_request(const model::Request& input) {
    Json messages = Json::array(), tools = Json::array();
    for (const auto& message : input.messages) messages.push_back(encode_message(message));
    for (const auto& tool : input.tools)
        tools.push_back({{"name",tool.name},{"description",tool.description},{"parameters",tool.parameters}});
    return {{"messages",std::move(messages)},{"tools",std::move(tools)},
        {"maxOutputTokens",input.max_output_tokens ? Json(*input.max_output_tokens) : Json(nullptr)}};
}

// 保存模型正文和用量，原始响应与后续有效响应由事件类型区分。
inline Json encode_response(const model::Response& output) {
    Json usage = nullptr;
    if (output.usage) {
        usage = {{"inputTokens",output.usage->input_tokens},{"outputTokens",output.usage->output_tokens}};
        if (output.usage->cache_hit_tokens) usage["cacheHitTokens"] = *output.usage->cache_hit_tokens;
        if (output.usage->cache_miss_tokens) usage["cacheMissTokens"] = *output.usage->cache_miss_tokens;
    }
    return {{"message",encode_message(output.message)},{"finish",finishes.at(static_cast<std::size_t>(output.finish))},
        {"usage",std::move(usage)}};
}
}
