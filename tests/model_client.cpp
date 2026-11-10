#include <dafeiyu/model_client.hpp>
#include <dafeiyu/http.hpp>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
using Json = nlohmann::json;
dfy::http::Response reply;
Json sent;
std::string endpoint;
int calls = 0;
const dfy::model::Config config{"deepseek", "https://example.invalid/v1", "test-model", "DFY_COMPLETION_TEST_KEY", 321};
const std::string prompt = "你好，\"大肥鱼\"\n第二行";

// 在调用行为不符合预期时报告测试失败。
void check(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}

// 验证调用被拒绝且错误不包含服务正文或测试凭据。
// 步骤：执行预期失败的请求 → 核对错误类别 → 检查敏感内容隔离。
void rejects(const std::string& input, const std::string& expected) {
    try {
        (void)dfy::model::complete(config, input);
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        check(message.find(expected) != std::string::npos, "unexpected completion error");
        check(message.find("private-test-token") == std::string::npos, "sensitive data leaked");
        return;
    }
    throw std::runtime_error("invalid completion accepted");
}
}

namespace dfy::http {
// 在协议测试中替代真实网络，记录请求并返回可控的服务结果。
// 步骤：核对传输参数并记录请求 → 模拟网络失败或返回服务响应。
Response post_json(const std::string& url, const std::string& key, const std::string& body, int timeout_ms) {
    ++calls;
    check(key == "private-test-token" && timeout_ms == 321, "credentials or timeout not forwarded");
    endpoint = url;
    sent = Json::parse(body);

    if (reply.status == 0) {
        throw std::runtime_error("network: timeout");
    }
    return reply;
}
}

// 离线验证双服务协议、凭据与输入边界，以及 HTTP、网络和响应失败路径。
// 步骤：检查有效请求 → 构造服务与响应错误 → 检查输入与凭据在联网前被拒绝。
int main() {
    try {
        check(_putenv_s(config.api_key_env.c_str(), "private-test-token") == 0, "cannot set test credential");
        const Json success{{"choices", Json::array({{{"finish_reason", "stop"},
                                           {"message", {{"role", "assistant"}, {"content", "你好！\n完整回答"}}}}})}};
        reply = {200, success.dump()};
        for (const auto* provider : {"deepseek", "qwen"}) {
            auto selected = config;
            selected.provider = provider;
            check(dfy::model::complete(selected, prompt) == "你好！\n完整回答", "response text changed");
            check(endpoint == config.base_url + "/chat/completions" && sent.at("model") == config.model &&
                      sent.at("stream") == false &&
                      sent.at("messages") == Json::array({{{"role", "user"}, {"content", prompt}}}),
                "request protocol mismatch");
            check(selected.provider == "deepseek"
                      ? sent.at("thinking").at("type") == "disabled" && !sent.contains("enable_thinking")
                      : sent.at("enable_thinking") == false && !sent.contains("thinking"),
                "provider options mixed");
        }

        for (unsigned status : {302u, 400u, 401u, 403u, 429u, 500u, 503u}) {
            reply = {status, "private-test-token"};
            rejects(prompt, "HTTP " + std::to_string(status));
        }
        for (const auto* malformed : {"private-test-token", "{}", "{\"choices\":[]}", "{\"choices\":[{}]}"}) {
            reply = {200, malformed};
            rejects(prompt, "invalid completion response");
        }
        for (const Json& reason : {Json("length"), Json("content_filter"), Json("tool_calls"), Json(nullptr)}) {
            auto result = success;
            result["choices"][0]["finish_reason"] = reason;
            reply = {200, result.dump()};
            rejects(prompt, "did not finish normally");
        }
        for (const Json& text : {Json(""), Json(nullptr), Json(42)}) {
            auto result = success;
            result["choices"][0]["message"]["content"] = text;
            reply = {200, result.dump()};
            rejects(prompt, text == "" ? "empty response text" : "invalid completion response");
        }
        reply = {0, {}};
        const auto before_timeout = calls;
        rejects(prompt, "network: timeout");
        check(calls == before_timeout + 1, "request retried");

        const auto before_invalid = calls;
        for (const auto& input : {std::string(), std::string(" \r\n\t"), std::string(1024 * 1024 + 1, 'x')}) {
            rejects(input, "prompt:");
        }
        rejects(std::string(1, static_cast<char>(0xFF)), "invalid UTF-8");
        check(_putenv_s(config.api_key_env.c_str(), "") == 0, "cannot clear test credential");
        rejects(prompt, "credentials: missing");
        check(_putenv_s(config.api_key_env.c_str(), "private-test-token\r\nInjected: x") == 0,
            "cannot set test credential");
        rejects(prompt, "credentials: expected");
        check(calls == before_invalid, "invalid input reached network");
        std::cout << "Model completion checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
