#include "application.hpp"

namespace dfy::app {
// 让正式会话与插件共享同一有界事件通道。
// 步骤：准备通道 → 创建插件目录与会话 → 绑定会话服务。
Application::Application(model::Config config, const std::filesystem::path& packages, const std::filesystem::path& user_data,
    model::Completion completion)
    : channel_(std::make_shared<events::BroadcastChannel>(std::make_shared<events::EventStore>(256,64),16,8)),
      plugins_(packages,user_data,channel_), session_(std::move(config),channel_,4,std::move(completion)),
      conversation_(session_) {}

// 确认会话已归还实际工作，再交给原管理器分步关闭。
plugins::ShutdownResult Application::close_plugins() noexcept {
    if (!conversation_.closed()) return plugins::ShutdownResult::busy;
    plugins_.close();
    return plugins_.close_status();
}
}
