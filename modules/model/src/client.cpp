#include <dafeiyu/model_client.hpp>
#include "thinking.hpp"
#include <dafeiyu/model_protocol.hpp>
#include <dafeiyu/http.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace dfy::model {
namespace {
// 读取配置所指凭据并执行一次 HTTPS 请求，不重试或回显服务错误正文。
// 步骤：读取并校验凭据 → 调用现有传输 → 检查 HTTP 状态 → 返回完整响应正文。
std::string send_request(const Config& config, const std::string& body) {
    const char* value = std::getenv(config.api_key_env.c_str());
    const std::string key = value ? value : "";
    if (key.empty()) throw std::runtime_error("credentials: missing environment variable " + config.api_key_env);
    if (std::any_of(key.begin(), key.end(), [](unsigned char c) { return c <= 32 || c >= 127; }))
        throw std::runtime_error("credentials: expected printable ASCII without whitespace");

    const auto response = http::post_json(config.base_url + "/chat/completions", key, body, config.timeout_ms);
    if (response.status != 200) throw std::runtime_error("model: HTTP " + std::to_string(response.status));
    return response.body;
}
}
// 请求结构化模型结果，保持协议校验在凭据和传输之前。
// 步骤：校验并编码统一请求 → 执行一次传输 → 解码并校验完整响应。
Response complete_response(const Config& config, const Request& request) {
    const auto body = encode_request(config,request);
    return decode_response(send_request(config,body));
}
// 将单次提示词包装为一条用户消息，复用统一的模型调用流程。
std::string complete(const Config& config, const std::string& prompt) {
    return complete(config, std::vector<Message>{{Role::user, prompt}});
}

// 读取运行时凭据并调用 DeepSeek 或 Qwen，只有正常结束的完整文本才作为成功返回。
// 步骤：检查消息与内容总量并构造请求 → 读取密钥 → 调用 HTTPS 接口 → 检查状态与响应内容。
std::string complete(const Config& config, const std::vector<Message>& messages) {
    using Json = nlohmann::json;
    if (messages.empty()) {
        throw std::runtime_error("messages: expected at least one message");
    }
    auto serialized = Json::array();
    std::size_t content_size = 0;
    for (const auto& message : messages) {
        if ((message.role != Role::user && message.role != Role::assistant) || !message.tool_calls.empty() || !message.tool_call_id.empty()) {
            throw std::runtime_error("messages: unsupported role or tool fields");
        }
        if (message.content.empty() ||
            (message.role == Role::user && message.content.find_first_not_of(" \t\r\n") == std::string::npos) ||
            message.content.size() > 1024 * 1024) {
            throw std::runtime_error("prompt: expected nonblank UTF-8 text within 1 MiB");
        }
        content_size += message.content.size();
        if (content_size > 1024 * 1024) {
            throw std::runtime_error("messages: total content exceeds 1 MiB");
        }
        serialized.push_back(
            {{"role", message.role == Role::user ? "user" : "assistant"}, {"content", message.content}});
    }

    Json request{{"model", config.model}, {"stream", false}, {"messages", std::move(serialized)}};
    if (!encode_thinking(request,config)) throw std::runtime_error("provider: expected deepseek or qwen");
    std::string body;
    try {
        body = request.dump();
    } catch (const Json::exception&) {
        throw std::runtime_error("prompt: invalid UTF-8 text");
    }

    const auto response = send_request(config,body);

    try {
        const auto result = Json::parse(response);
        const auto& choice = result.at("choices").at(0);
        if (choice.at("finish_reason") != "stop") {
            throw std::runtime_error("model: response did not finish normally");
        }
        const auto text = choice.at("message").at("content").get<std::string>();
        if (text.empty()) {
            throw std::runtime_error("model: empty response text");
        }
        return text;
    } catch (const Json::exception&) {
        // 服务错误和解析错误不回显响应正文，以免间接暴露凭据或输入内容。
        throw std::runtime_error("model: invalid completion response");
    }
}
}
