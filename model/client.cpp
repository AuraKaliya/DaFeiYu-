#include <dafeiyu/model_client.hpp>
#include <dafeiyu/http.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdlib>
#include <stdexcept>

namespace dfy::model {
// 读取运行时凭据并调用 DeepSeek 或 Qwen，只有正常结束的完整文本才作为成功返回。
// 步骤：检查输入并构造服务请求 → 读取密钥 → 调用 HTTPS 接口 → 检查状态与响应内容。
std::string complete(const Config& config, const std::string& prompt) {
    using Json = nlohmann::json;
    if (prompt.find_first_not_of(" \t\r\n") == std::string::npos || prompt.size() > 1024 * 1024) {
        throw std::runtime_error("prompt: expected nonblank UTF-8 text within 1 MiB");
    }

    Json request{{"model", config.model}, {"stream", false},
        {"messages", Json::array({{{"role", "user"}, {"content", prompt}}})}};
    if (config.provider == "deepseek") {
        request["thinking"] = {{"type", "disabled"}};
    } else if (config.provider == "qwen") {
        request["enable_thinking"] = false;
    } else {
        throw std::runtime_error("provider: expected deepseek or qwen");
    }
    std::string body;
    try {
        body = request.dump();
    } catch (const Json::exception&) {
        throw std::runtime_error("prompt: invalid UTF-8 text");
    }

    const char* value = std::getenv(config.api_key_env.c_str());
    const std::string key = value ? value : "";
    if (key.empty()) {
        throw std::runtime_error("credentials: missing environment variable " + config.api_key_env);
    }
    if (std::any_of(key.begin(), key.end(), [](unsigned char c) { return c <= 32 || c >= 127; })) {
        throw std::runtime_error("credentials: expected printable ASCII without whitespace");
    }

    const auto response = http::post_json(config.base_url + "/chat/completions", key, body, config.timeout_ms);
    if (response.status != 200) {
        throw std::runtime_error("model: HTTP " + std::to_string(response.status));
    }

    try {
        const auto result = Json::parse(response.body);
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
