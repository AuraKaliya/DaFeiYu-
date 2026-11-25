#include <dafeiyu/model_protocol.hpp>
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>

namespace {
using namespace dfy::model;
using Json = nlohmann::json;
// 将协议断言失败转换为进程失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 确认非法响应被拒绝且错误不包含服务端正文。
void rejects(const Json& document) {
    try { (void)decode_response(document.dump()); }
    catch (const std::exception& error) {
        check(std::string(error.what()).find("private-response") == std::string::npos,"response body leaked"); return;
    }
    throw std::runtime_error("invalid structured response accepted");
}
// 构造带工具调用和用量的固定服务响应，不访问凭据或服务。
Json fixture() {
    return {{"choices",Json::array({{{"finish_reason","tool_calls"},{"message",{{"role","assistant"},{"content",nullptr},
        {"tool_calls",Json::array({{{"id","call_1"},{"type","function"},{"function",{{"name","read_state"},{"arguments","{}"}}}}})}}}}})},
        {"usage",{{"prompt_tokens",12},{"completion_tokens",4}}}};
}
// 验证两家供应商参数隔离和调用结果往返，旧文本请求不附带工具字段。
// 步骤：编码两家相同业务请求 → 检查工具消息及定义 → 解码完整与异常响应。
void protocol() {
    Request request{{{Role::system,"rules"},{Role::user,"goal"},{Role::assistant,"",{{"call_1","read_state","{}"}}},
        {Role::tool,"state",{},"call_1"}},{{"read_state","read","{\"type\":\"object\"}"}},64};
    for (const auto* provider : {"deepseek","qwen"}) {
        Config config; config.provider = provider; config.model = "fixture-model";
        const auto body = Json::parse(encode_request(config,request));
        check(body["stream"] == false && body["max_tokens"] == 64 && body["messages"][3]["tool_call_id"] == "call_1" &&
            body["tools"][0]["function"]["parameters"]["type"] == "object","structured request changed");
        check(config.provider == "deepseek" ? body.contains("thinking") && !body.contains("enable_thinking") :
            body.contains("enable_thinking") && !body.contains("thinking"),"provider dialect leaked");
        const auto plain = Json::parse(encode_request(config,{{{Role::user,"plain"}}}));
        check(!plain.contains("tools") && !plain.contains("max_tokens"),"text request gained optional fields");
    }
    const auto response = decode_response(fixture().dump());
    check(response.finish == FinishReason::tool_calls && response.message.tool_calls[0].name == "read_state" &&
        response.usage && response.usage->input_tokens == 12 && response.usage->output_tokens == 4,"response lost calls or usage");
    auto document = fixture(); document.erase("usage");
    check(!decode_response(document.dump()).usage,"missing usage invented token counts");
    document = fixture();
    check(!decode_response(document.dump()).usage->cache_hit_tokens,"missing cache hit count became zero");
    document["usage"]["prompt_cache_hit_tokens"] = 0;
    document["usage"]["prompt_cache_miss_tokens"] = 12;
    const auto cache = decode_response(document.dump()).usage;
    check(cache->cache_hit_tokens == 0 && cache->cache_miss_tokens == 12,"provider cache counters lost");
    document = fixture(); document["choices"][0]["finish_reason"] = "length"; rejects(document);
    document = fixture(); document["usage"]["prompt_tokens"] = -1; rejects(document);
    document = fixture(); document["choices"][0]["message"]["tool_calls"][0]["function"]["arguments"] = "private-response"; rejects(document);
    document = fixture(); document["choices"][0]["message"]["role"] = "user"; rejects(document);
    document = fixture(); document["choices"][0]["finish_reason"] = "unknown"; rejects(document);
    document = fixture(); document["choices"][0] = {{"finish_reason","stop"},{"message",{{"role","assistant"},{"content","answer"}}}};
    check(decode_response(document.dump()).message.content == "answer","plain response lost text");
}

// 用录制形状验证 DeepSeek 在带工具回合延续供应商推理字段。
// 步骤：解码固定工具响应 → 编码续行及跨轮历史 → 检查关闭和无工具边界。
void thinking_protocol() {
    auto recorded = fixture();
    recorded["choices"][0]["message"]["reasoning_content"] = "fixed inference before tool";
    const auto response = decode_response(recorded.dump());
    check(response.message.reasoning_content == "fixed inference before tool","tool continuation was not captured");
    Request request{{{Role::user,"previous"},{Role::assistant,"answer",{},{},"fixed inference before text"},
        {Role::user,"goal"},response.message,{Role::tool,"state",{},"call_1"}},
        {{"read_state","read","{\"type\":\"object\"}"}},64};
    Config config;
    config.provider = "deepseek";
    config.thinking = true;
    auto body = Json::parse(encode_request(config,request));
    check(!body.contains("reasoning_effort") && body["thinking"]["type"] == "enabled" && body["messages"][1]["reasoning_content"] == "fixed inference before text" &&
        body["messages"][3]["reasoning_content"] == "fixed inference before tool" &&
        body["messages"][4]["tool_call_id"] == "call_1","thinking continuation lost original ordering");
    config.thinking = false;
    config.session_thinking = ThinkingPreference::low;
    body = Json::parse(encode_request(config,request));
    check(body.at("reasoning_effort") == "low" && body.at("thinking").at("type") == "enabled" &&
        body.at("messages")[3].at("reasoning_content") == "fixed inference before tool","low effort lost tool continuation");
    config.thinking = true;
    config.session_thinking = ThinkingPreference::off;
    body = Json::parse(encode_request(config,request));
    check(body.at("thinking").at("type") == "disabled" && !body.contains("reasoning_effort"),"explicit off lost its scope");
    check(!body["messages"][1].contains("reasoning_content") && !body["messages"][3].contains("reasoning_content"),
        "thinking disabled request forwarded reasoning");
    config.session_thinking = ThinkingPreference::inherit;
    config.thinking = true;
    request.messages.resize(3);
    request.tools.clear();
    check(!Json::parse(encode_request(config,request))["messages"][1].contains("reasoning_content"),
        "request without tools forwarded reasoning");
    recorded["choices"][0]["message"]["reasoning_content"] = 42;
    rejects(recorded);
}
}
// 只执行固定协议样本并报告结果。
int main() {
    try { protocol(); thinking_protocol(); std::cout << "Model protocol checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
