#include "browser_lifetime.hpp"
#include <algorithm>

namespace dfy::ui {
// 取得环境自己的进程快照，并在进程仍存在时持有同步句柄。
// 步骤：查询环境能力与进程集合 → 限制数量 → 去重并打开内核对象。
void BrowserProcesses::observe(ICoreWebView2Environment* environment) {
    ComPtr<ICoreWebView2Environment8> source; require_hresult(environment->QueryInterface(IID_PPV_ARGS(&source)));
    ComPtr<ICoreWebView2ProcessInfoCollection> list; require_hresult(source->GetProcessInfos(&list));
    UINT32 count = 0; require_hresult(list->get_Count(&count));
    if (count > 64) throw std::length_error("UI: unexpected browser process count");
    for (UINT32 index = 0; index < count; ++index) {
        ComPtr<ICoreWebView2ProcessInfo> item; require_hresult(list->GetValueAtIndex(index,&item));
        INT32 id = 0; require_hresult(item->get_ProcessId(&id));
        if (id <= 0) throw std::runtime_error("UI: invalid browser process identity");
        if (std::any_of(processes_.begin(),processes_.end(),[id](const auto& item) { return item.id == static_cast<DWORD>(id); })) continue;
        if (processes_.size() == 64) throw std::length_error("UI: retained process limit reached");
        Handle handle(OpenProcess(SYNCHRONIZE,FALSE,static_cast<DWORD>(id)),&CloseHandle);
        if (!handle) {
            if (GetLastError() == ERROR_INVALID_PARAMETER) continue;
            throw std::runtime_error("UI: cannot observe browser process exit");
        }
        processes_.push_back({static_cast<DWORD>(id),std::move(handle)});
    }
}
// 每次只探测句柄，不等待并阻塞宿主消息循环。
bool BrowserProcesses::exited() const noexcept {
    return std::all_of(processes_.begin(),processes_.end(),[](const auto& process) { return WaitForSingleObject(process.handle.get(),0) == WAIT_OBJECT_0; });
}
// 使用系统临时目录和新 GUID 创建本次配置，已经存在的目标明确拒绝。
// 步骤：建立专用根并校验边界 → 签发目录名称 → 创建并记录所有权。
void ProbeProfile::create() {
    namespace fs = std::filesystem;
    auto root = fs::temp_directory_path()/L"DaFeiYu-U01";
    fs::create_directories(root);
    if (GetFileAttributesW(root.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("UI: profile root must not be a reparse point");
    root_ = fs::canonical(root);
    GUID id{}; require_hresult(CoCreateGuid(&id)); wchar_t name[40]{};
    if (!StringFromGUID2(id,name,40)) throw std::runtime_error("UI: cannot name profile");
    path_ = root_/name;
    if (!fs::create_directory(path_)) throw std::runtime_error("UI: profile already exists");
    owned_ = true;
}
// 只清理原来创建的普通目录，失败可在进程和文件锁退出后再次尝试。
// 步骤：核对所有权及解析后绝对路径 → 拒绝树内重解析点 → 在同一目录树内删除。
bool ProbeProfile::cleanup() noexcept {
    namespace fs = std::filesystem;
    if (!owned_) return true;
    try {
        if (!fs::exists(path_)) { owned_ = false; return true; }
        if (fs::canonical(path_) != path_ || path_.parent_path() != root_ || !fs::is_directory(path_)) return false;
        if (GetFileAttributesW(path_.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) return false;
        for (const auto& entry : fs::recursive_directory_iterator(path_))
            if (GetFileAttributesW(entry.path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) return false;
        fs::remove_all(path_); owned_ = false; return true;
    } catch (...) { return false; }
}
}
