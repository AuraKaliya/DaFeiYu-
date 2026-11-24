#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace dfy::model {
enum class ThinkingPreference { inherit, off, low };
struct Config {
    std::string provider;
    std::string base_url;
    std::string model;
    std::string api_key_env;
    int timeout_ms = 0;
    bool thinking = false;
    ThinkingPreference session_thinking = ThinkingPreference::inherit;

    // 按供应商能力和本轮选择解析实际思考开关。
    bool thinking_enabled() const noexcept {
        return provider == "deepseek" && (session_thinking == ThinkingPreference::low ||
            (session_thinking == ThinkingPreference::inherit && thinking));
    }
    // 返回实际发给供应商的强度值，空值表示沿用协议默认。
    std::string_view reasoning_effort() const noexcept {
        return provider == "deepseek" && session_thinking == ThinkingPreference::low ? "low" : "";
    }
};

// 离线读取并校验单个模型服务配置，保留密钥环境变量引用而不读取凭据。
Config load_config(const std::filesystem::path& path);
}
