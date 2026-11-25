#include "tray_icon.hpp"
#include "tray_resource.h"
#include <utility>

namespace dfy::ui {
namespace {
constexpr wchar_t window_class[] = L"DaFeiYu.ResidentTray";
constexpr UINT icon_message = WM_APP + 17;

// 仅在可见窗口站调用本桌面的 Shell，服务窗口站没有通知区域。
BOOL notify_desktop(DWORD action, NOTIFYICONDATAW* icon) {
    USEROBJECTFLAGS station{};
    if (!GetUserObjectInformationW(GetProcessWindowStation(), UOI_FLAGS, &station, sizeof(station), nullptr)
        || !(station.dwFlags & WSF_VISIBLE)) return FALSE;
    return Shell_NotifyIconW(action, icon);
}
}

// 为本实例建立隐藏顶层窗口，非交互窗口站保留后台运行。
// 步骤：注册广播和窗口 → 按桌面尺寸加载图像 → 将图标交给通知区域。
TrayIcon::TrayIcon(Report report_sink, Notify notify)
    : notify_(notify ? std::move(notify) : Notify{notify_desktop}), report_(std::move(report_sink)) {
    const auto instance = GetModuleHandleW(nullptr);
    taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW kind{};
    kind.lpfnWndProc = procedure;
    kind.hInstance = instance;
    kind.lpszClassName = window_class;
    if (taskbar_created_ && (RegisterClassW(&kind) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS)) {
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW, window_class, L"大肥鱼", WS_POPUP,
            0, 0, 0, 0, nullptr, nullptr, instance, this);
    }
    if (!window_) {
        report(TrayEvent::added, false);
        return;
    }

    const auto dpi = GetDpiForWindow(window_);
    icon_.cbSize = sizeof(icon_);
    icon_.hWnd = window_;
    icon_.uID = 1;
    icon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    icon_.uCallbackMessage = icon_message;
    wcscpy_s(icon_.szTip, L"大肥鱼");
    icon_.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_DAFEIYU), IMAGE_ICON,
        GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CYSMICON, dpi), LR_DEFAULTCOLOR));
    publish(TrayEvent::added);
}

// 先撤销已登记的身份，再归还创建线程的窗口和图像句柄。
// 步骤：向 Shell 注销 → 销毁隐藏窗口 → 释放图像资源。
TrayIcon::~TrayIcon() {
    if (added_) report(TrayEvent::removed, notify_(NIM_DELETE, &icon_) != FALSE);
    if (window_) DestroyWindow(window_);
    if (icon_.hIcon) DestroyIcon(icon_.hIcon);
}

// 将实际系统回执交给拥有本体日志的调用方。
void TrayIcon::report(TrayEvent event, bool success) const {
    if (report_) report_(event, success);
}

// 对仍存在的图标只更新属性，Explorer 丢失槽位后重新注册并声明回调协议。
// 步骤：探测原槽位 → 注册应用图标 → 设置回调版本并报告结果。
void TrayIcon::publish(TrayEvent event) {
    if (added_ && notify_(NIM_MODIFY, &icon_)) return;
    added_ = icon_.hIcon && notify_(NIM_ADD, &icon_);
    if (added_) {
        icon_.uVersion = NOTIFYICON_VERSION_4;
        if (!notify_(NIM_SETVERSION, &icon_)) {
            notify_(NIM_DELETE, &icon_);
            added_ = false;
        }
    }
    report(event, added_);
}

// 将真实的任务栏广播交给所属图标，隐藏窗口不参与普通关窗退出。
// 步骤：绑定创建参数 → 处理任务栏重建 → 交给默认窗口过程。
LRESULT CALLBACK TrayIcon::procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
    }
    auto* self = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self && message == self->taskbar_created_) {
        self->publish(TrayEvent::restored);
        return 0;
    }
    if (message == WM_CLOSE) return 0;
    return DefWindowProcW(window, message, wparam, lparam);
}
}
