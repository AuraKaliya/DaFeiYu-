#pragma once
#include <cstdint>
#include <string>

namespace dfy::app {
struct HostSettingsView {
    std::string provider;
    int timeout_ms = 0, applied_timeout_ms = 0;
    std::uint64_t revision = 1, applied_revision = 1;
    bool user_timeout = false, can_save = false;
};

// 只持有管理界面登记的主体字段，运行值固定在本体采用配置时。
class HostSettings {
  public:
    // 从本次启动实际采用的供应商和超时建立主体设置首个修订。
    HostSettings(const std::string& provider, int timeout_ms) : view_{provider,timeout_ms,timeout_ms} {}
    // 返回同一时刻的已保存值和主体实际采用值。
    HostSettingsView snapshot() const { return view_; }
  private:
    HostSettingsView view_;
};
}
