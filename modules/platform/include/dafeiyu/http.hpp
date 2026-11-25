#pragma once
#include <string>

namespace dfy::http {
struct Response {
    unsigned status;
    std::string body;
};

// 同步向 ASCII HTTPS 地址提交 JSON，携带 Bearer 凭据，返回状态码并仅在 200 时读取正文。
Response post_json(const std::string& url, const std::string& api_key, const std::string& body, int timeout_ms);
}
