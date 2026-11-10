#pragma once
#include <dafeiyu/core.hpp>
#include <dafeiyu/schema.hpp>
#include <chrono>
#include <functional>
#include <map>
#include <set>
#include <vector>

namespace dfy {
using SteadyClock = std::chrono::steady_clock;
// 取得系统时钟的当前毫秒时间戳。
int64_t now_ms();
// 生成供实例、状态对象及临时令牌使用的随机标识。
std::string identifier();
// 计算文本的稳定指纹，用于比较配置或构造工作标识。
std::string fingerprint(std::string_view text);
// 读取限定大小的文件内容，并拒绝打开或读取失败的文件。
std::string read_text(const Path& path);
// 读取文件并解析为 JSON 值。
Json read_json(const Path& path);
// 在条件不满足时抛出携带指定错误码的异常。
void require(bool condition, const std::string& code);
// 判断 JSON 集合是否包含指定值。
bool includes(const Json& values, const Json& value);
// 判断运行任务是否已完成、失败或取消。
bool terminal(const Json& run);
// 将核心异常转换为带有类别和重试提示的标准错误记录。
Json failure(const std::exception& error);
// 校验操作结果的状态、副作用、覆盖范围及必要字段。
void validate_outcome(const Json& value);
}
