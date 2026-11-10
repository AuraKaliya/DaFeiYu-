#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace dfy::plugins {
struct Manifest {
    std::filesystem::path directory;
    std::string id;
    std::string name;
    std::string version;
    std::uint32_t abi_version;
    std::filesystem::path entry;
    std::optional<std::filesystem::path> default_config;
};

// 读取目录内版本 1 清单并校验包内文件，返回规范化绝对路径；不加载 DLL，失败抛出异常。
Manifest load_manifest(const std::filesystem::path& directory);
}
