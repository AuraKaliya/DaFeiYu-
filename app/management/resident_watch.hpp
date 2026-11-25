#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace dfy::management {
struct ResidentObservation {
    std::uint32_t pid = 0;
    bool requested = false;
    std::optional<std::uint32_t> exit_code;
    std::uint32_t error = 0;
};

// 本体拥有的一次性退出意图，已连接窗口持有同一个内核事件的等待权限。
class StopSignal {
  public:
    // 仅为当前实例新建手动复位事件，拒绝使用同名旧对象。
    explicit StopSignal(std::string_view instance);
    // 归还本体句柄，已连接观察者仍能核对此次退出意图。
    ~StopSignal();
    // 首次显式接纳时置位，重复接纳返回假且保持事件置位。
    bool accept();
  private:
    struct State;
    std::unique_ptr<State> state_;
};

// 握手期间保留原管道对端的进程对象和退出意图，后续不按 PID 重新打开。
class ResidentWatch {
  public:
    // 借用已核实的进程句柄，在握手回执发回前取得独立的只读观察权。
    ResidentWatch(void* process, std::uint32_t pid, std::string_view instance, bool has_signal);
    // 释放对原实例的观察句柄。
    ~ResidentWatch();
    // 零等待采样内核事实，进程对象置位后才读取实际退出码。
    ResidentObservation observe() const noexcept;
  private:
    struct State;
    std::unique_ptr<State> state_;
};
}
