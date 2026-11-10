#pragma once
#include <dafeiyu/model_config.hpp>

namespace dfy::model {
// 使用 load_config 校验后的配置和 UTF-8 提示词同步请求一次完整文本，失败时抛出异常。
std::string complete(const Config& config, const std::string& prompt);
}
