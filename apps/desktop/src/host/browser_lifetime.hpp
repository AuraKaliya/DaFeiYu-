#pragma once
#include "webview_support.hpp"
#include <filesystem>
#include <vector>

namespace dfy::ui {
// 仅观察当前 WebView2 环境返回的进程句柄，不按进程名终止或控制其他应用。
class BrowserProcesses {
  public:
    // 收集当前环境内进程的同步句柄，失败不冒充已经退出。
    void observe(ICoreWebView2Environment* environment);
    // 非阻塞检查所有已观察内核对象是否结束，PID 复用不改变持有对象。
    bool exited() const noexcept;
    // 返回持有的进程对象数，句柄在观察器销毁时归还。
    std::size_t count() const noexcept { return processes_.size(); }
  private:
    using Handle = std::unique_ptr<void,decltype(&CloseHandle)>;
    struct Process { DWORD id; Handle handle; };
    std::vector<Process> processes_;
};
// 每次验证创建全新配置，清理只限本实例创建且仍位于指定临时根下的目录。
class ProbeProfile {
  public:
    // 创建唯一空目录，不复用浏览器或其他验证进程的用户数据。
    void create();
    // 在确认进程退出后尝试清理，路径或重解析边界异常则保留并返回失败。
    bool cleanup() noexcept;
    // 返回本次环境使用的配置路径。
    const std::filesystem::path& path() const noexcept { return path_; }
    // 查询本实例创建的目录是否已经完成清理。
    bool cleaned() const noexcept { return !owned_; }
  private:
    std::filesystem::path root_, path_;
    bool owned_ = false;
};
}
