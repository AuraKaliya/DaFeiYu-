#pragma once
#include <dafeiyu/model_config.hpp>
#include <dafeiyu/model_messages.hpp>
#include <vector>

namespace dfy::model {
// 使用内置供应商请求完整结构化响应；调用工具由 Agent 处理，不在模型层执行。
Response complete_response(const Config& config, const Request& request);

// 使用已校验配置和有序 UTF-8 消息同步请求完整文本，失败时抛出异常。
std::string complete(const Config& config, const std::vector<Message>& messages);

// 使用 load_config 校验后的配置和 UTF-8 提示词同步请求一次完整文本，失败时抛出异常。
std::string complete(const Config& config, const std::string& prompt);
}
