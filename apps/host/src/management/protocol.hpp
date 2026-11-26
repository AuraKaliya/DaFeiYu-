#pragma once
#include "application.hpp"
#include <nlohmann/json_fwd.hpp>

namespace dfy::management {
class Host;
// UTF-8 控制字符编码可扩至六字节，快照另给协议字段保留空间。
inline constexpr std::size_t request_limit = 6*app::dialogue_text_limit+512;
inline constexpr std::size_t response_limit = 6*(app::dialogue_display_byte_limit+app::dialogue_queue_byte_limit)+16384;
// 校验并处理管理协议请求，返回脱离窗口的可传输快照或原操作回执。
nlohmann::json dispatch(Host* host, const nlohmann::json& request, const std::string& client = {});
// 严格解析管理协议的规范十进制修订、命令号或代次，完整保留 64 位精度。
std::uint64_t management_number(const nlohmann::json& value);
// 返回指定后台已登记的主体设置与实际生效修订。
nlohmann::json settings_request(Host& host, const nlohmann::json& request, const std::string& client);
}
