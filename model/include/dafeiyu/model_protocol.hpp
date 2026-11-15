#pragma once
#include <dafeiyu/model_config.hpp>
#include <dafeiyu/model_messages.hpp>

namespace dfy::model {
// 将统一请求编码为对应供应商的非流式请求，校验失败不读取凭据。
std::string encode_request(const Config& config, const Request& request);
// 将完整供应商响应解码为统一结果，不回显响应正文或隐藏推理。
Response decode_response(const std::string& body);
}
