#pragma once
#include <dafeiyu/plugin_manifest.hpp>
#include <cstddef>
#include <vector>

namespace dfy::plugins {
struct PackageLimits { std::size_t entries = 4096, bytes = 512*1024*1024, depth = 32; };
// 校验并暂存本地目录包，完整复核后以目录改名发布；拒绝覆盖，不加载 DLL 或修改用户配置。
Manifest install_package(const std::filesystem::path& root, const std::filesystem::path& source, PackageLimits limits = {});
struct RemovalReceipt {
    bool found = false;
    std::vector<std::filesystem::path> residual;
    bool package_present = false, more_pending = false;
};
// 在实例退出后按与安装一致的限制分批清理及移除；回执区分仍可见包和未处理批次。
RemovalReceipt remove_package(const std::filesystem::path& root, const std::string& id, PackageLimits limits = {});
}
