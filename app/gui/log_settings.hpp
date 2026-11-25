#pragma once
#include "../software_log.hpp"
#include <nlohmann/json_fwd.hpp>
#include <functional>

namespace dfy::ui {
using FolderOpener = std::function<unsigned long(const std::filesystem::path&)>;
// 在客户端工作线程向 Windows 提交打开既定目录的操作，零表示 Shell 已接纳。
unsigned long open_directory(const std::filesystem::path& folder);
// 核实指定实例的日志目录，再将打开操作提交给所选 Shell 入口。
unsigned long open_log_folder(const std::filesystem::path& profile, const FolderOpener& opener);
// 读取本窗口的写入状态，仅对本数据目录下实际存在的日志目录调用打开入口。
nlohmann::json log_settings(const std::filesystem::path& profile, const app::SoftwareLog& log, bool open, const FolderOpener& opener);
}
