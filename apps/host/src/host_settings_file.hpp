#pragma once
#include "host_settings.hpp"

namespace dfy::app {
// 有界读取并校验主体用户文件，未建立文件时返回空覆盖记录。
std::deque<HostSettingWrite> read_host_settings_file(const std::filesystem::path& file);
// 以同目录临时文件原子发布完整超时配置及其有界回执，失败保留原目标。
void write_host_settings_file(const std::filesystem::path& file, const std::deque<HostSettingWrite>& writes, bool replacing);
}
