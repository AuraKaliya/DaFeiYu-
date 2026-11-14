#pragma once
#include <dafeiyu/plugin_manifest.hpp>
#include <cstddef>
#include <vector>

namespace dfy::plugins {
struct PackageLimits { std::size_t entries = 4096, bytes = 512*1024*1024, depth = 32; };
// 校验并暂存本地目录包，完整复核后以目录改名发布；拒绝覆盖，不加载 DLL 或修改用户配置。
Manifest install_package(const std::filesystem::path& root, const std::filesystem::path& source, PackageLimits limits = {});
struct RemovalReceipt { bool found = false; std::vector<std::filesystem::path> residual; };
// 在调用方确认实例退出后，将唯一包移出可发现目录再清理；重复调用只重试同 ID 私有残留。
RemovalReceipt remove_package(const std::filesystem::path& root, const std::string& id);
}
