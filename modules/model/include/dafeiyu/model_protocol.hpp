#pragma once
#include <dafeiyu/model_config.hpp>
#include <dafeiyu/model_messages.hpp>

namespace dfy::model {
// 按供应商使用的相同编码取得消息、工具和输出限额，供本地对照及容量核算。
std::string encode_request_fields(const Request& request, bool thinking);
// 将统一请求编码为对应供应商的非流式请求，校验失败不读取凭据。
std::string encode_request(const Config& config, const Request& request);
// 将完整供应商响应解码为统一结果，不回显响应正文或隐藏推理。
Response decode_response(const std::string& body);
}
