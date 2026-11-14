#pragma once
#include <filesystem>
#include <vector>
#include <dafeiyu/plugin_package.hpp>

namespace dfy::plugins::files {
using Limits = PackageLimits;
struct Item { std::filesystem::path relative; bool directory; };
// 逐层校验本地目录路径，拒绝任何重解析点；可明确创建缺失目录，返回规范绝对路径。
std::filesystem::path directory(const std::filesystem::path& path, bool create = false);
// 有界检查整棵包目录，只接受普通文件及目录，不跟随链接或设备路径。
std::vector<Item> inspect(const std::filesystem::path& root, Limits limits = {});
// 在已确认根内创建不可被注册表发现的唯一直接子目录。
std::filesystem::path temporary(const std::filesystem::path& root, const std::string& prefix);
// 核对来源与目标均为根的直接子目录后原子改名，不覆盖已有目标。
void move(const std::filesystem::path& root, const std::filesystem::path& source, const std::filesystem::path& destination);
// 按明确限制清理根下私有直接子目录，逐项重新核对实际路径后删除，不调用无边界递归删除。
void erase(const std::filesystem::path& root, const std::filesystem::path& target, Limits limits = {});
}
