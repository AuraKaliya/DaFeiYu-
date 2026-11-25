#include "resident_tray.hpp"
#include "tray_icon.hpp"
#include "panel_activation.hpp"
#include "exit_confirmation.hpp"
#include "paths.hpp"
#include "log_settings.hpp"
#include <nlohmann/json.hpp>
#include <thread>

namespace dfy::ui {
namespace {
// 在独立的 Windows 消息线程上拥有图标，宿主释放它时等待真实注销。
class DesktopTray final : public management::ResidentTray {
  public:
    // 保留原本体日志的借用权，线程在日志和目录所有权之前退出。
    DesktopTray(std::filesystem::path profile, std::string instance, std::string definition, app::SoftwareLog* log)
        : thread_([profile=std::move(profile), instance=std::move(instance), definition=std::move(definition), log](std::stop_token stop) {
            run(stop, profile, instance, definition, log);
        }) {}
  private:
    // 在创建窗口的线程处理广播与释放，后台的管道循环可独立工作。
    // 步骤：建立图标并登记回执 → 绑定退出唤醒 → 推进消息直到本体收尾。
    static void run(std::stop_token stop, const std::filesystem::path& profile, const std::string& instance,
        const std::string& definition, app::SoftwareLog* log) noexcept {
        try {
            SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            PanelActivation panels(app::executable_directory() / L"dafeiyu-ui-probe.exe", profile, definition, instance);
            HWND owner = nullptr;
            ExitConfirmation confirmation([&] {
                if (stop.stop_requested()) return;
                try { (void)management::request_stop(profile, instance, {}, log); }
                catch (const std::exception&) {
                    // 原实例的失败回执已由 request_stop 登记。
                }
            });
            TrayIcon icon([log](TrayEvent event, bool ok) {
                if (!log) return;
                auto type = app::SoftwareEvent::tray_start;
                if (event == TrayEvent::restored) type = app::SoftwareEvent::tray_restored;
                if (event == TrayEvent::removed) type = app::SoftwareEvent::tray_removed;
                log->record(type, ok ? app::SoftwareResult::ok : app::SoftwareResult::failed);
            }, {},
            // 菜单意图只作用于当前目录及原实例，退出仍需用户确认。
            // 步骤：分派面板或日志操作 → 缺少面板时提供本机确认 → 记录实际接纳结果。
            [&](TrayCommand command) {
                if (stop.stop_requested()) return;
                const auto type = command == TrayCommand::open_logs ? app::SoftwareEvent::tray_open_logs :
                    command == TrayCommand::open_panel ? app::SoftwareEvent::tray_open_panel : app::SoftwareEvent::tray_confirm_exit;
                DWORD error = ERROR_GEN_FAILURE;
                try {
                    error = command == TrayCommand::open_logs ? open_log_folder(profile, open_directory) :
                        panels.open(command == TrayCommand::confirm_exit);
                    if (command == TrayCommand::confirm_exit && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND))
                        error = confirmation.show(owner);
                } catch (const std::system_error& failure) { error = failure.code().value(); }
                catch (...) {}
                if (log) log->record(type, error ? app::SoftwareResult::failed : app::SoftwareResult::accepted,
                    {.system_error=error});
            });
            if (icon.window()) {
                owner = icon.window();
                SetWindowTextW(icon.window(), std::wstring(instance.begin(), instance.end()).c_str());
                std::stop_callback wake(stop, [window = icon.window()] { PostMessageW(window, WM_CANCELMODE, 0, 0); });
                MSG message{};
                while (!stop.stop_requested() && GetMessageW(&message, nullptr, 0, 0) > 0) {
                    if (message.hwnd == icon.window() && message.message == panel_ready)
                        panels.ready(reinterpret_cast<HWND>(message.wParam));
                    if (confirmation.translate(message)) continue;
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
management::StartResidentTray resident_tray_factory(std::string definition) {
    return [definition=std::move(definition)](const auto& profile, const auto& instance, app::SoftwareLog* log) {
        return std::make_unique<DesktopTray>(profile, instance, definition, log);
    };
}
}
