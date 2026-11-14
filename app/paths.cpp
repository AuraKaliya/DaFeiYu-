#include "paths.hpp"
#include <windows.h>
#include <shlobj.h>
#include <memory>
#include <stdexcept>

namespace dfy::app {
// 有界读取当前程序路径，截断或系统错误不退回不明确的工作目录。
std::filesystem::path executable_directory() {
    std::wstring path(32768,L'\0'); const auto length = GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) throw std::runtime_error("app: cannot locate executable directory");
    path.resize(length); return std::filesystem::path(path).parent_path();
}
// 通过系统已知文件夹定位用户设置，保持包目录与用户配置物理分离。
std::filesystem::path default_user_data() {
    PWSTR raw = nullptr; const auto status = SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_DEFAULT,nullptr,&raw);
    std::unique_ptr<wchar_t,decltype(&CoTaskMemFree)> path(raw,&CoTaskMemFree);
    if (FAILED(status) || !path) throw std::runtime_error("app: cannot locate user data directory");
    return std::filesystem::path(path.get())/"DaFeiYu";
}
}
