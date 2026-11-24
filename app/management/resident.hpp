#pragma once
#include "host.hpp"
#include <nlohmann/json_fwd.hpp>

namespace dfy::management {
// 将有效启动目录和配置路径编码为连接双方可核对的启动标识。
std::string startup_identity(const std::filesystem::path& model, const std::filesystem::path& packages,
    const std::filesystem::path& trace = {});
// 独占目录端点并运行本体，客户端离开后继续推进，显式关闭后返回实际退出结果。
int serve_resident(const std::filesystem::path& profile, const std::string& identity, Host::Open open, std::stop_token stop = {});
// 通过本地通道发送受版本约束的请求，拒绝、断连及超时以异常返回。
nlohmann::json resident_request(const std::filesystem::path& profile, nlohmann::json request);
}
