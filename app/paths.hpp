#pragma once
#include <filesystem>
namespace dfy::app {
// 返回实际运行程序所在目录，资源定位不依赖当前工作目录。
std::filesystem::path executable_directory();
// 返回当前 Windows 用户的本地应用数据根，不创建或修改目录。
std::filesystem::path default_user_data();
}
