#pragma once
#include <nlohmann/json_fwd.hpp>

namespace dfy::management {
class Host;
// 在受信管理协议内按原后台、来源及修订查询公开目录、信件与收件回执。
nlohmann::json mailbox_request(const Host& host, const nlohmann::json& request);
}
