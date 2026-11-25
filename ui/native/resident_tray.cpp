#include "resident_tray.hpp"
#include "tray_icon.hpp"
#include <thread>

namespace dfy::ui {
namespace {
// 在独立的 Windows 消息线程上拥有图标，宿主释放它时等待真实注销。
class DesktopTray final : public management::ResidentTray {
  public:
    // 保留原本体日志的借用权，线程在日志和目录所有权之前退出。
    explicit DesktopTray(app::SoftwareLog* log) : thread_([log](std::stop_token stop) { run(stop, log); }) {}
  private:
    // 在创建窗口的线程处理广播与释放，后台的管道循环可独立工作。
    // 步骤：建立图标并登记回执 → 绑定退出唤醒 → 推进消息直到本体收尾。
    static void run(std::stop_token stop, app::SoftwareLog* log) noexcept {
        try {
            SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            TrayIcon icon([log](TrayEvent event, bool ok) {
                if (!log) return;
                auto type = app::SoftwareEvent::tray_start;
                if (event == TrayEvent::restored) type = app::SoftwareEvent::tray_restored;
                if (event == TrayEvent::removed) type = app::SoftwareEvent::tray_removed;
                log->record(type, ok ? app::SoftwareResult::ok : app::SoftwareResult::failed);
            });
            if (icon.window()) {
                std::stop_callback wake(stop, [window = icon.window()] { PostMessageW(window, WM_CANCELMODE, 0, 0); });
                MSG message{};
                while (!stop.stop_requested() && GetMessageW(&message, nullptr, 0, 0) > 0) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
        } catch (...) {
            if (log) log->record(app::SoftwareEvent::tray_start, app::SoftwareResult::failed);
        }
    }
    std::jthread thread_;
};
}

// 每次已取得目录所有权的本体只创建一个托盘所有者。
std::unique_ptr<management::ResidentTray> start_resident_tray(const std::filesystem::path&,
    const std::string&, app::SoftwareLog* log) {
    return std::make_unique<DesktopTray>(log);
}
}
