#include "panel_activation.hpp"
#include "tray_icon.hpp"

namespace dfy::ui {
namespace {
// 取得创建窗口的进程编号，窗口已销毁时返回零。
DWORD process_id(HWND window) noexcept {
    DWORD pid = 0;
    if (window) GetWindowThreadProcessId(window, &pid);
    return pid;
}

// 只向已握手到当前进程的面板投递激活意图。
bool activate(HWND window, bool confirm) {
    if (reinterpret_cast<ULONG_PTR>(GetPropW(window, panel_owner)) != GetCurrentProcessId()) return false;
    AllowSetForegroundWindow(process_id(window));
    return PostMessageW(window, activate_panel, confirm, 0) != FALSE;
}
}

// 按原实例签发的名称核实托盘进程，避免迟到的新窗连接同目录的替代本体。
DWORD tray_owner(const std::wstring& instance) noexcept {
    return instance.empty() ? 0 : process_id(FindWindowW(tray_window_class, instance.c_str()));
}

// 公布已验证的后台进程，只有托盘新开的窗口需要回复启动回执。
bool publish_panel(HWND window, DWORD owner, const std::wstring& instance) noexcept {
    if (!owner || !SetPropW(window, panel_owner, reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(owner)))) return false;
    if (!instance.empty()) {
        const auto tray = FindWindowW(tray_window_class, instance.c_str());
        if (process_id(tray) == owner) PostMessageW(tray, panel_ready, reinterpret_cast<WPARAM>(window), 0);
    }
    return true;
}

// 在本体仍持有原路径时固定启动参数，避免依赖后续的当前目录。
PanelActivation::PanelActivation(std::filesystem::path program, std::filesystem::path profile,
    const std::string& definition, const std::string& instance)
    : program_(std::move(program)), profile_(std::filesystem::absolute(profile)),
      definition_(definition.begin(), definition.end()), instance_(instance.begin(), instance.end()) {}

// 优先唤回属于此本体的窗口，在途启动只接纳一次并保留退出确认意图。
// 步骤：按顶层窗口的原进程标记寻找 → 合并在途操作 → 以明确的程序和参数创建面板。
DWORD PanelActivation::open(bool confirm) {
    HWND window = nullptr;
    while ((window = FindWindowExW(nullptr, window, panel_window_class, nullptr))) {
        if (activate(window, confirm || confirm_pending_)) {
            launching_.reset();
            confirm_pending_ = false;
            return ERROR_SUCCESS;
        }
    }
    if (launching_ && WaitForSingleObject(launching_.get(), 0) == WAIT_TIMEOUT) {
        confirm_pending_ |= confirm;
        return ERROR_SUCCESS;
    }

    // 有效 Windows 目录名不含双引号，末尾的点确保根目录反斜杠不会转义闭引号。
    auto command = L"dafeiyu-ui-probe.exe --resident-panel " + definition_ + L" " + instance_ +
        (confirm ? L" confirm" : L" open") + L" --data \"" + (profile_ / L".").wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(program_.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
        return GetLastError();
    CloseHandle(process.hThread);
    launching_.reset(process.hProcess);
    confirm_pending_ = false;
    return ERROR_SUCCESS;
}

// 只结算本次启动句柄对应的原窗口，其他面板的迟到回执不能替换它。
void PanelActivation::ready(HWND window) {
    if (!launching_ || process_id(window) != GetProcessId(launching_.get()) ||
        reinterpret_cast<ULONG_PTR>(GetPropW(window, panel_owner)) != GetCurrentProcessId()) return;
    if (confirm_pending_) activate(window, true);
    confirm_pending_ = false;
    launching_.reset();
}
}
