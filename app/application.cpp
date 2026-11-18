#include "application.hpp"
#include <dafeiyu/model_client.hpp>

namespace dfy::app {
// 装配内置结构化 Provider，普通问答沿用同一后端和上下文。
Application::Application(model::Config config, const std::filesystem::path& packages, const std::filesystem::path& user_data)
    : Application(std::move(config),packages,user_data,model::Backend{model::complete_response}) {}

// 让正式会话与插件共享同一有界事件通道。
// 步骤：准备通道 → 创建插件目录与会话 → 绑定会话服务。
Application::Application(model::Config config, const std::filesystem::path& packages, const std::filesystem::path& user_data,
    model::Backend backend)
    : channel_(std::make_shared<events::BroadcastChannel>(std::make_shared<events::EventStore>(256,64),16,8)),
      plugins_(packages,user_data,channel_), session_(std::move(config),channel_,std::move(backend)),
      conversation_(session_) {}

// 从用户指定的已加载实例授予一个只读工具。
// 步骤：核对会话及工具身份 → 准备原实例注册 → 校验只读范围并接纳 → 保留注册直到退出或撤销。
agent::TurnDiagnostics Application::submit_read_only(const std::string& tool_id, const std::string& prompt) {
    if (conversation_.busy()) throw std::logic_error("session: wait for the current request to finish");
    const auto separator = tool_id.find('/');
    if (separator == std::string::npos || !separator || separator+1 == tool_id.size())
        throw std::invalid_argument("tool: expected package/tool ID");

    const auto instance = plugins_.instance(tool_id.substr(0,separator));
    if (instance->phase() != plugins::InstancePhase::active) throw std::invalid_argument("tool: plugin instance is not active");
    const auto catalog = std::make_shared<agent::ToolCatalog>(64);
    auto registration = std::make_unique<plugins::PluginTools>(instance,catalog);
    agent::RunOptions options;
    options.tools = catalog->snapshot(std::span(&tool_id,1));

    const auto accepted = conversation_.submit(prompt,std::move(options));
    tool_registration_.emplace(ToolRegistration{accepted.turn,instance,std::move(registration)});
    return accepted;
}

// 归还本轮工具注册并推进插件自己的异步工作。
// 步骤：核对原运行和实例是否仍有效 → 撤销不再使用的注册 → 推进插件管理器。
runtime::RunResult Application::advance_plugins() {
    if (tool_registration_) {
        const auto status = conversation_.status();
        const auto instance = tool_registration_->instance.lock();
        if (!status || status->turn != tool_registration_->turn || status->exit != runtime::OperationState::pending ||
            !instance || instance->phase() != plugins::InstancePhase::active) tool_registration_.reset();
    }
    return plugins_.advance();
}

// 确认会话已归还实际工作，再交给原管理器分步关闭。
plugins::ShutdownResult Application::close_plugins() noexcept {
    if (!conversation_.closed()) return plugins::ShutdownResult::busy;
    tool_registration_.reset();
    plugins_.close();
    return plugins_.close_status();
}
}
