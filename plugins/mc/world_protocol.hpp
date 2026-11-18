#pragma once
#include <string>
#include <string_view>

namespace dfy::mc {
// 从独立插件配置中读取本机管道短名。
std::string configured_channel(std::string_view config);
// 创建带原请求身份的只读查询。
std::string world_request(std::string_view id);
// 核对版本、回显和状态形状，返回规范快照文本或表示撤销的 null。
std::string world_reply(std::string_view wire, std::string_view expected_id);
}
