#pragma once
#include <filesystem>
#include <string>

namespace dfy::model {
struct Config {
    std::string provider;
    std::string base_url;
    std::string model;
    std::string api_key_env;
    int timeout_ms;
};

// 离线读取并校验单个模型服务配置，保留密钥环境变量引用而不读取凭据。
Config load_config(const std::filesystem::path& path);
}
