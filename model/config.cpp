#include <dafeiyu/model_config.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <regex>
#include <set>
#include <stdexcept>

namespace dfy::model {
namespace {
using Json = nlohmann::json;

// 读取必填文本字段，拒绝空值、空白和控制字符。
std::string text_field(const Json& object, const char* name) {
    const auto field = object.find(name);
    if (field == object.end() || !field->is_string()) {
        throw std::runtime_error(std::string(name) + ": required string");
    }
    const auto value = field->get<std::string>();
    if (value.empty() || value.find_first_of(" \t\r\n") != std::string::npos ||
        std::any_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; })) {
        throw std::runtime_error(std::string(name) + ": must be nonempty and contain no whitespace or controls");
    }
    return value;
}

// 校验 HTTPS 服务基址的域名、可选端口和路径前缀，并去掉尾部斜杠。
// 步骤：匹配基址结构 → 检查端口范围 → 规范化路径并拒绝完整调用地址。
std::string base_url(std::string value) {
    static const std::regex pattern(
        R"(https://[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?)*(?::([0-9]{1,5}))?(/[A-Za-z0-9._~/-]*)?)");
    std::smatch match;
    if (!std::regex_match(value, match, pattern) ||
        (match[1].matched && (std::stoi(match[1]) == 0 || std::stoi(match[1]) > 65535))) {
        throw std::runtime_error(
            "base_url: expected https://host[:port][/prefix] without credentials, query or fragment");
    }

    while (value.ends_with('/')) {
        value.pop_back();
    }
    if (value.ends_with("/chat/completions")) {
        throw std::runtime_error("base_url: use the API prefix, not the /chat/completions endpoint");
    }
    return value;
}
}

// 离线加载 DeepSeek 或 Qwen 配置，并返回与 JSON 实现无关的模型参数。
// 步骤：读取并解析文件 → 检查对象字段 → 校验服务、地址及密钥引用 → 校验超时并返回配置。
Config load_config(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("config: cannot open file");
    }
    Json object;
    std::set<std::string> keys;
    try {
        object = Json::parse(file, [&](int depth, Json::parse_event_t event, Json& value) {
            if (depth == 1 && event == Json::parse_event_t::key && !keys.insert(value.get<std::string>()).second) {
                throw std::runtime_error("config: duplicate field");
            }
            return true;
        });
    } catch (const Json::exception&) {
        // 解析器的原始错误可能包含文件内容，只报告格式错误，避免回显误填的凭据。
        throw std::runtime_error("config: invalid JSON");
    }
    if (file.bad()) {
        throw std::runtime_error("config: cannot read file");
    }

    const std::set<std::string> allowed{"provider", "base_url", "model", "api_key_env", "timeout_ms"};
    if (!object.is_object()) {
        throw std::runtime_error("config: expected an object");
    }
    for (const auto& key : keys) {
        if (!allowed.contains(key)) {
            throw std::runtime_error("config: unknown field");
        }
    }

    Config config{text_field(object, "provider"), base_url(text_field(object, "base_url")), text_field(object, "model"),
        text_field(object, "api_key_env"), 60000};
    if (config.provider != "deepseek" && config.provider != "qwen") {
        throw std::runtime_error("provider: expected deepseek or qwen");
    }
    if (!std::regex_match(config.api_key_env, std::regex("[A-Za-z_][A-Za-z0-9_]*"))) {
        throw std::runtime_error("api_key_env: expected an environment variable name");
    }

    if (object.contains("timeout_ms")) {
        const auto& timeout = object.at("timeout_ms");
        if (!timeout.is_number_integer() || timeout < 1 || timeout > 600000) {
            throw std::runtime_error("timeout_ms: expected an integer from 1 to 600000");
        }
        config.timeout_ms = timeout.get<int>();
    }
    return config;
}
}
