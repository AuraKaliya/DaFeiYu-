#include "tray_icon.hpp"
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

namespace {
using dfy::ui::TrayEvent;
using dfy::ui::TrayIcon;

// 将实际不符的系统行为报告给 CTest。
void expect(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

struct Shell {
    HWND slot = nullptr;
    bool available = true, version_supported = true, valid = true;
    int adds = 0, versions = 0, deletes = 0;
    // 只模拟 Explorer 拥有的槽位；窗口、注册广播、图像和消息分派均走 Win32。
    // 步骤：核对原生身份 → 模拟登记或查询 → 核对版本或注销回执。
    BOOL notify(DWORD action, const NOTIFYICONDATAW& icon) {
        valid &= icon.cbSize == sizeof(icon) && IsWindow(icon.hWnd) && icon.uID == 1;
        if (action == NIM_ADD) {
            if (!available || slot) return FALSE;
            valid &= icon.hIcon && (icon.uFlags & NIF_MESSAGE) && icon.uCallbackMessage >= WM_APP;
            slot = icon.hWnd;
            ++adds;
            return TRUE;
        }
        if (!slot || slot != icon.hWnd) return FALSE;
        if (action == NIM_SETVERSION) {
            ++versions;
            return version_supported && icon.uVersion == NOTIFYICON_VERSION_4;
        }
        if (action == NIM_DELETE) { ++deletes; slot = nullptr; return TRUE; }
        return action == NIM_MODIFY;
    }
};

// 将重建广播投给真实顶层窗口，核对 Shell 延后上线、重复通知与正常释放。
// 步骤：使首次登记失败 → 恢复后模拟 Explorer 丢失槽位 → 重发广播并析构。
void lifecycle() {
    Shell shell;
    shell.available = false;
    std::vector<std::pair<TrayEvent, bool>> events;
    const UINT rebuilt = RegisterWindowMessageW(L"TaskbarCreated");
    HWND window = nullptr;
    {
        TrayIcon icon([&](TrayEvent event, bool ok) { events.emplace_back(event, ok); },
            [&](DWORD action, NOTIFYICONDATAW* data) { return shell.notify(action, *data); });
        window = icon.window();
        expect(window && GetParent(window) == nullptr && !IsWindowVisible(window), "expected hidden top-level window");
        expect(events.size() == 1 && events.back() == std::pair{TrayEvent::added, false}, "missing initial failure");
        shell.available = true;
        SendMessageW(window, rebuilt, 0, 0);
        expect(shell.slot == window && shell.adds == 1 && shell.versions == 1, "first recovery failed");
        shell.slot = nullptr;
        SendMessageW(window, rebuilt, 0, 0);
        SendMessageW(window, rebuilt, 0, 0);
        expect(shell.adds == 2 && shell.versions == 2 && events.size() == 3, "duplicate or missing recovery");
        expect(events.back() == std::pair{TrayEvent::restored, true}, "missing restoration receipt");
    }
    expect(!IsWindow(window) && !shell.slot && shell.deletes == 1 && shell.valid, "native resources not released");
    expect(events.back() == std::pair{TrayEvent::removed, true}, "missing deletion receipt");

    shell.version_supported = false;
    TrayIcon rejected({}, [&](DWORD action, NOTIFYICONDATAW* data) { return shell.notify(action, *data); });
    expect(shell.valid && !shell.slot && shell.deletes == 2, "failed negotiation left an unusable icon");
}

// 在有 Explorer 的桌面通过真实 Shell 独立探测图标的存在与移除。
// 步骤：检查系统任务栏 → 核对真实图标身份 → 确认退出后的 Shell 回执。
int desktop() {
    if (!FindWindowW(L"Shell_TrayWnd", nullptr)) return 77;
    // 用系统图标独立确认当前进程有桌面接入权，避免把权限不足误报为应用注册失败。
    HWND control = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr);
    expect(control != nullptr, "could not create a control window");
    NOTIFYICONDATAW baseline{};
    baseline.cbSize = sizeof(baseline);
    baseline.hWnd = control;
    baseline.uID = 1;
    baseline.uFlags = NIF_ICON;
    baseline.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    const auto available = Shell_NotifyIconW(NIM_ADD, &baseline);
    if (available) Shell_NotifyIconW(NIM_DELETE, &baseline);
    DestroyWindow(control);
    if (!available) { std::cerr << "Explorer declined the independent system icon\n"; return 77; }
    NOTIFYICONDATAW probe{};
    bool added = false, removed = false;
    {
        TrayIcon icon([&](TrayEvent event, bool success) {
            if (event == TrayEvent::added) added = success;
            if (event == TrayEvent::removed) removed = success;
        });
        probe.cbSize = sizeof(probe);
        probe.hWnd = icon.window();
        probe.uID = 1;
        expect(added, "Explorer did not accept the icon registration");
        expect(Shell_NotifyIconW(NIM_MODIFY, &probe), "Explorer could not find the registered icon");
    }
    expect(removed && !Shell_NotifyIconW(NIM_MODIFY, &probe), "Explorer still owns the icon");
    return 0;
}

// 在独立的非交互窗口站核对生产注册失败不会阻止本体继续。
// 步骤：切换窗口站 → 创建并释放原生组件 → 恢复原窗口站。
int headless() {
    const auto original = GetProcessWindowStation();
    const auto station = CreateWindowStationW(nullptr, 0, WINSTA_ALL_ACCESS, nullptr);
    if (!station && GetLastError() == ERROR_ACCESS_DENIED) {
        std::cerr << "This process cannot create a private window station\n";
        return 77;
    }
    if (!station) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "create private window station");
    if (!SetProcessWindowStation(station))
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "enter private window station");
    bool unavailable = false;
    {
        TrayIcon icon([&](TrayEvent event, bool success) { unavailable = event == TrayEvent::added && !success; });
    }
    expect(SetProcessWindowStation(original) && CloseWindowStation(station), "could not release private window station");
    expect(unavailable, "noninteractive station reported an icon");
    return 0;
}
}

// 独立验证通知区域和无交互桌面的实际生命周期。
// 步骤：选择隔离场景 → 运行原生断言 → 输出首个失败原因。
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--headless") return headless();
        if (argc == 2 && std::string_view(argv[1]) == "--explorer") return desktop();
        lifecycle();
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
