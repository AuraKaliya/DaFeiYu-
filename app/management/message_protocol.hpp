#pragma once
#include <nlohmann/json_fwd.hpp>
#include <string>

namespace dfy::management {
class Host;
// 按真实客户端登记主会话订阅，读取固定水位分页或指定提交回执。
nlohmann::json message_request(Host& host, const nlohmann::json& request, const std::string& client);
}
