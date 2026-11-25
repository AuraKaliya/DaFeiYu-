#pragma once
#include <windows.h>
#include <shellapi.h>
#include <functional>

namespace dfy::ui {
enum class TrayEvent { added, restored, removed };

// 在创建线程拥有一个接收任务栏广播的顶层窗口及其 Shell 图标。
class TrayIcon {
  public:
    using Notify = std::function<BOOL(DWORD, NOTIFYICONDATAW*)>;
    using Report = std::function<void(TrayEvent, bool)>;
    // 在当前线程创建隐藏窗口，Shell 暂不可用时保留窗口等待任务栏重建。
    explicit TrayIcon(Report report = {}, Notify notify = {});
    // 在同一线程注销自己的图标，随后释放窗口和图像。
    ~TrayIcon();
    // 窗口借用原对象地址，图标不能复制。
    TrayIcon(const TrayIcon&) = delete;
    // 保留已注册窗口与其唯一所有者的绑定。
    TrayIcon& operator=(const TrayIcon&) = delete;
    // 返回接收原生消息的窗口，无可用桌面时为空。
    HWND window() const noexcept { return window_; }
  private:
    // 分派原生广播，窗口只在对象存活期间借用其地址。
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    // 登记图标，任务栏广播重复到达时只更新既有槽位。
    void publish(TrayEvent event);
    // 记录已取得的 Shell 结果。
    void report(TrayEvent event, bool success) const;
    Notify notify_;
    Report report_;
    HWND window_ = nullptr;
    NOTIFYICONDATAW icon_{};
    UINT taskbar_created_ = 0;
    bool added_ = false;
};
}
