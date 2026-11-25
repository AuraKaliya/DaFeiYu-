#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

namespace dfy::app {
enum class SoftwareRole { host, window, terminal };
enum class SoftwareEvent { process_started, process_exiting, host_listening, host_launch, client_connected,
    client_disconnected, stop_submitted, stop_accepted, stop_reply, profile_claim, configuration_read,
    window_closing, webview_released, profile_cleaned, window_destroyed, conversation_drain, plugins_drain };
enum class SoftwareResult { started, ok, failed, accepted, busy };
struct SoftwareFields {
    std::string_view instance{}, client{};
    bool shutdown = false;
    std::optional<unsigned long> system_error{};
    std::optional<int> exit_code{};
    std::optional<std::int64_t> duration_ms{};
    std::optional<std::uint64_t> count{};
    std::uint64_t session = 0, turn = 0;
};

// 每次产品进程启动独立记录软件生命周期，写盘失败与业务结算分别报告。
class SoftwareLog {
  public:
    // 启动专用写入线程，将首条事件投到当前数据目录的 logs 下。
    SoftwareLog(const std::filesystem::path& profile, SoftwareRole role) noexcept;
    // 等待已接纳的诊断写完，再关闭本进程持有的文件。
    ~SoftwareLog();
    // 为后续事件保存本进程已经核实的原本体与可选观察者身份。
    void bind(std::string_view instance, std::string_view client = {}) noexcept;
    // 仅接纳已声明的软件事件、宿主签发的身份和数值字段。
    void record(SoftwareEvent event, SoftwareResult result, SoftwareFields fields = {}) noexcept;
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
