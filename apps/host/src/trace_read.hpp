#pragma once
#include <dafeiyu/run_identity.hpp>
#include <filesystem>
#include <functional>
#include <nlohmann/json_fwd.hpp>

namespace dfy::app {
// 按磁盘顺序读取格式及序号连续的记录，返回是否以完整行结束。
bool read_trace(const std::filesystem::path& file, const std::function<void(const nlohmann::json&)>& visit);
// 判断记录是否拥有同一原会话与回合。
bool matches_run(const nlohmann::json& record, agent::RunId run);
}
