#pragma once
#include <windows.h>
#include <filesystem>
#include <memory>
#include <string>

namespace dfy::ui {
inline constexpr wchar_t panel_window_class[] = L"DaFeiYuU01";
inline constexpr wchar_t panel_owner[] = L"DaFeiYu.Management.ResidentPid";
inline constexpr UINT activate_panel = WM_APP + 18, panel_ready = WM_APP + 19;
// 返回本次托盘窗口所在的进程，原实例已经结束时返回零。
DWORD tray_owner(const std::wstring& instance) noexcept;
// 在握手后将可唤回窗口标记为指定后台所有，并通知等待启动的托盘。
bool publish_panel(HWND window, DWORD owner, const std::wstring& instance) noexcept;

// 合并面板启动期间的连续点击，将后续操作交给核实过进程身份的窗口。
class PanelActivation {
  public:
    // 保存本体指定的绝对数据目录、配置身份和实例号。
    PanelActivation(std::filesystem::path program, std::filesystem::path profile,
        const std::string& definition, const std::string& instance);
    // 唤回同一本体的现有窗口，或启动只允许连接原本体的面板。
    DWORD open(bool confirm);
    // 已启动的窗口完成握手后释放启动句柄，并补交在途的确认请求。
    void ready(HWND window);
  private:
    std::filesystem::path program_, profile_;
    std::wstring definition_, instance_;
    std::unique_ptr<void, decltype(&CloseHandle)> launching_{nullptr, &CloseHandle};
    bool confirm_pending_ = false;
};
}
