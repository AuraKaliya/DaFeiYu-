#pragma once
#include <dafeiyu/model_config.hpp>
#include <filesystem>
#include <optional>
#include <span>

namespace dfy::app { class SoftwareLog; }
namespace dfy::management {
struct ResidentPaths {
    std::filesystem::path model, plugins, data, trace;
};
// 解析本体和客户端共用的路径参数，缺省资源按程序目录定位。
ResidentPaths resident_paths(std::optional<std::filesystem::path> model, std::span<const std::filesystem::path> options);
// 读取运行配置并登记成功或失败，日志不含路径与配置正文。
model::Config read_runtime_config(const std::filesystem::path& file, app::SoftwareLog& log);
// 执行显式的常驻、状态或退出命令，以原后台实例的结果返回退出码。
int resident_command(std::span<const std::filesystem::path> args);
}
