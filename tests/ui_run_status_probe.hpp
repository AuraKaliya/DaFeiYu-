#pragma once
#include "application.hpp"
#include <atomic>

namespace dfy::tests {
// 从原生验收序列驱动同一状态面板任务，页面只参与状态查询。
class UiRunStatusProbe {
  public:
    // 保存 CTest 选择的真实状态面板包。
    explicit UiRunStatusProbe(std::filesystem::path package) : package_(std::move(package)) {}
    // 在业务线程装配固定响应和真实插件。
    std::unique_ptr<app::Application> open(const std::filesystem::path& work);
    // 在业务线程接收验收序列发出的本地操作。
    void drive(app::Application& application);
    // 在空闲快照已经验证后投递测试任务。
    void submit() noexcept { action_ = Action::submit; }
    // 在等待和拒绝快照已经验证后投递本地批准。
    void approve() noexcept { action_ = Action::approve; }
  private:
    enum class Action { none, submit, approve };
    const std::filesystem::path package_;
    std::atomic<Action> action_ = Action::none;
    std::atomic_uint requests_ = 0;
};
}
