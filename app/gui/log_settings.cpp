#include "log_settings.hpp"
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <nlohmann/json.hpp>

namespace dfy::ui {
// 在客户端工作线程提交 Shell 操作，保留系统接纳或失败的真实结果。
// 步骤：建立工作线程 COM 公寓 → 提交固定目录 → 取回错误码并归还公寓。
unsigned long open_directory(const std::filesystem::path& folder) {
    const auto init = CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);
    if (FAILED(init)) return static_cast<DWORD>(init);
    SHELLEXECUTEINFOW action{};
    action.cbSize = sizeof(action);
    action.fMask = SEE_MASK_FLAG_NO_UI|SEE_MASK_NOASYNC;
    action.lpVerb = L"open";
    action.lpFile = folder.c_str();
    action.nShow = SW_SHOWNORMAL;
    const auto error = ShellExecuteExW(&action) ? ERROR_SUCCESS : GetLastError();
    CoUninitialize();
    return error;
}

// 向受信设置页提供本进程写入证据和固定目录的打开回执。
// 步骤：从启动配置计算目录 → 读取日志线程状态 → 按显式请求核对目录并提交给 Shell。
nlohmann::json log_settings(const std::filesystem::path& profile, const app::SoftwareLog& log, bool open, const FolderOpener& opener) {
    const auto folder = std::filesystem::absolute(profile/"logs").lexically_normal();
    const auto name = folder.u8string();
    nlohmann::json result{{"folder",std::string(name.begin(),name.end())},{"writeError",log.error()},{"written",log.recorded()}};
    if (open) {
        const auto attrs = GetFileAttributesW(folder.c_str());
        DWORD error = ERROR_SUCCESS;
        if (attrs==INVALID_FILE_ATTRIBUTES) error = GetLastError();
        else if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) error = ERROR_DIRECTORY;
        else error = opener(folder);
        result["openError"] = error;
    }
    return result;
}
}
