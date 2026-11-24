#pragma once
#include <dafeiyu/model_config.hpp>
#include <nlohmann/json.hpp>

namespace dfy::model {
// 在文本请求与工具请求中采用同一供应商思考方言和本轮配置。
inline bool encode_thinking(nlohmann::json& body, const Config& config) {
    if (config.provider == "deepseek") {
        body["thinking"] = {{"type",config.thinking_enabled() ? "enabled" : "disabled"}};
        if (const auto effort = config.reasoning_effort(); !effort.empty()) body["reasoning_effort"] = effort;
    } else if (config.provider == "qwen") body["enable_thinking"] = false;
    else return false;
    return true;
}
}
