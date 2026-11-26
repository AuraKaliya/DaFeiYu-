#pragma once
#include <nlohmann/json_fwd.hpp>

namespace dfy::management {
class Host;
// 在受信管理协议内按原后台读取已发布的来源目录。
nlohmann::json mailbox_request(const Host& host, const nlohmann::json& request);
}
