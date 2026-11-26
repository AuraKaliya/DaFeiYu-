#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

namespace dfy::app {
inline constexpr int host_timeout_min = 1, host_timeout_max = 600000;
inline constexpr std::size_t settings_receipt_limit = 32;
struct HostSettingsView {
    std::string provider;
    int timeout_ms = 0, applied_timeout_ms = 0;
    std::uint64_t revision = 1, applied_revision = 1;
    bool user_timeout = false, can_save = false, available = true;
};
struct HostSettingWrite {
    std::string submission;
    std::uint64_t revision = 0;
    int timeout_ms = 0;
    // 比较已写入磁盘的原命令与对应配置修订。
    bool operator==(const HostSettingWrite&) const = default;
};
struct HostSaveResult {
    std::string_view state;
    std::uint64_t saved_revision = 0;
    HostSettingsView current;
    int saved_timeout_ms = 0;
};
// 只持有管理界面登记的主体字段，运行值固定在本体采用配置时。
class HostSettings {
  public:
    // 在创建会话前读取正式后台的用户覆盖并更新启动超时，未绑定目录的应用沿用传入值。
    HostSettings(const std::filesystem::path& profile, const std::string& provider, int& startup_timeout);
    // 返回同一时刻的已保存值和主体实际采用值。
    HostSettingsView snapshot() const;
    // 串行核对同一主体的原提交与基准修订，只在用户文件完整发布后报告已保存。
    HostSaveResult save(const std::string& submission, std::uint64_t base, int timeout_ms);
    // 按原提交号读取仍保留的已保存修订，支持管理窗口重新连接及本体重启。
    HostSaveResult receipt(const std::string& submission) const;
  private:
    mutable std::mutex mutex_;
    std::filesystem::path file_;
    std::deque<HostSettingWrite> writes_;
    HostSettingsView view_;
};
// 核对原提交标识的长度和可打印 ASCII 字符。
bool valid_settings_submission(std::string_view text) noexcept;
}
