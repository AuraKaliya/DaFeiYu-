#pragma once
#include <dafeiyu/model_config.hpp>
#include <vector>

namespace dfy::model {
enum class Role { user, assistant };
struct Message {
    Role role;
    std::string content;
};

// 使用已校验配置和有序 UTF-8 消息同步请求完整文本，失败时抛出异常。
std::string complete(const Config& config, const std::vector<Message>& messages);

// 使用 load_config 校验后的配置和 UTF-8 提示词同步请求一次完整文本，失败时抛出异常。
std::string complete(const Config& config, const std::string& prompt);
}
