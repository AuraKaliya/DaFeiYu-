#include <dafeiyu/model_config.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using Json = nlohmann::json;

// 在配置行为不符合预期时报告测试失败。
void check(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}

// 写入隔离配置文件并核对拒绝原因，检查错误不回显配置中的敏感标记。
// 步骤：写入临时配置 → 验证加载失败 → 检查错误类别及敏感信息隔离。
void rejects(const std::filesystem::path& path, const std::string& body, const std::string& expected) {
    {
        std::ofstream file(path, std::ios::binary);
        file << body;
    }

    try {
        (void)dfy::model::load_config(path);
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        check(message.find(expected) != std::string::npos, "unexpected validation error");
        check(message.find("private-test-token") == std::string::npos, "configuration content leaked");
        return;
    }
    throw std::runtime_error("invalid configuration accepted");
}
}

// 验证两个服务示例、可配置参数、字段边界和非法文件的离线拒绝行为。
// 步骤：加载正式示例 → 检查默认值与地址规范化 → 构造非法配置并验证错误 → 检查缺失文件。
int main(int argc, char** argv) {
    try {
        check(argc == 3, "expected configuration and work directories");
        const std::filesystem::path examples(argv[1]), work(argv[2]);
        std::filesystem::create_directories(work);
        const auto path = work / u8"配置 测试.json";
        for (const auto* provider : {"deepseek", "qwen"}) {
            const auto config = dfy::model::load_config(examples / (std::string(provider) + ".example.json"));
            check(config.provider == provider && config.timeout_ms == 60000, "example configuration changed");
            check(!config.base_url.empty() && !config.model.empty() && !config.api_key_env.empty(),
                "missing model settings");
        }

        Json valid{{"provider", "qwen"},
            {"base_url", "https://workspace.cn-beijing.maas.aliyuncs.com:443/compatible-mode/v1/"},
            {"model", "future-model"}, {"api_key_env", "DFY_CONFIG_TEST_KEY"}};
        {
            std::ofstream file(path, std::ios::binary);
            file << valid.dump();
        }
        auto loaded = dfy::model::load_config(path);
        check(loaded.timeout_ms == 60000 && !loaded.base_url.ends_with('/'), "default or normalization failed");
        check(
            loaded.model == "future-model" && loaded.api_key_env == "DFY_CONFIG_TEST_KEY", "settings were overwritten");
        for (int timeout : {1, 600000}) {
            auto value = valid;
            value["timeout_ms"] = timeout;
            {
                std::ofstream file(path);
                file << value.dump();
            }
            check(dfy::model::load_config(path).timeout_ms == timeout, "valid timeout boundary rejected");
        }

        for (const auto* field : {"provider", "base_url", "model", "api_key_env"}) {
            auto missing = valid;
            missing.erase(field);
            rejects(path, missing.dump(), field);
            for (const Json& wrong : {Json(nullptr), Json(42), Json(""), Json("bad value"), Json("bad\nvalue")}) {
                auto value = valid;
                value[field] = wrong;
                rejects(path, value.dump(), field);
            }
        }
        for (const Json& timeout :
            {Json(0), Json(-1), Json(600001), Json(18446744073709551615ULL), Json(1.5), Json(true), Json("1000")}) {
            auto value = valid;
            value["timeout_ms"] = timeout;
            rejects(path, value.dump(), "timeout_ms");
        }
        for (const auto* address : {"http://example.com", "https://", "https://-host", "https://example.com:0",
                 "https://example.com:65536", "https://user:private-test-token@example.com",
                 "https://example.com?key=private-test-token", "https://example.com/#fragment",
                 "https://{WorkspaceId}.example.com", "https://example.com/v1/chat/completions/"}) {
            auto value = valid;
            value["base_url"] = address;
            rejects(path, value.dump(), "base_url");
        }
        auto value = valid;
        value["provider"] = "other";
        rejects(path, value.dump(), "provider");
        value = valid;
        value["api_key_env"] = "1INVALID";
        rejects(path, value.dump(), "api_key_env");
        value = valid;
        value["api_key"] = "private-test-token";
        rejects(path, value.dump(), "unknown field");
        rejects(path, "[]", "expected an object");
        rejects(path, "{\"provider\":\"qwen\",\"provider\":\"deepseek\"}", "duplicate field");
        rejects(path, "{\"api_key\": private-test-token}", "invalid JSON");
        rejects(path, "{\"timeout_ms\":1e999}", "invalid JSON");

        try {
            (void)dfy::model::load_config(work / "absent" / "model.json");
            throw std::logic_error("missing file accepted");
        } catch (const std::runtime_error& error) {
            check(std::string(error.what()) == "config: cannot open file", "unexpected file error");
        }
        std::cout << "Model configuration checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
