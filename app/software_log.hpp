#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace dfy::app {
enum class SoftwareRole { host, window, terminal };
enum class SoftwareEvent { process_started, process_exiting };
enum class SoftwareResult { started, ok, failed };

// 每次产品进程启动独立记录软件生命周期，写盘失败与业务结算分别报告。
class SoftwareLog {
  public:
    // 启动专用写入线程，将首条事件投到当前数据目录的 logs 下。
    SoftwareLog(const std::filesystem::path& profile, SoftwareRole role) noexcept;
    // 等待已接纳的诊断写完，再关闭本进程持有的文件。
    ~SoftwareLog();
    // 仅接纳已声明的软件事件和数值字段，正文与异常文本不进入日志。
    void record(SoftwareEvent event, SoftwareResult result, std::optional<int> exit_code = {}) noexcept;
    // 登记本进程准备返回的退出码，实际退出由持有进程句柄的一方确认。
    int exiting(int code) noexcept;
    // 返回首次写入或初始化失败的错误码，零表示尚未发现错误。
    unsigned long error() const noexcept;
    // 至少一条完整记录已经写入且刷新后返回真。
    bool recorded() const noexcept;
  private:
    struct State;
    std::unique_ptr<State> state_;
};
}
