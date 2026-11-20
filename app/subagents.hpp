#pragma once
#include <dafeiyu/plugin_manager.hpp>
#include <dafeiyu/plugin_tools.hpp>
#include <dafeiyu/plugin_agents.hpp>
#include <dafeiyu/session.hpp>
#include <array>

namespace dfy::app {
class RunLog;
using plugins::SubagentInput;
using plugins::SubagentPhase;
using plugins::SubagentStatus;

// 宿主按插件代次持有独立会话，插件提交与模型或工具执行之间隔着推进边界。
class Subagents final : public plugins::PluginAgents {
  public:
    // 固定共享后端、通知、时钟及原插件目录。
    Subagents(plugins::PluginManager& plugins, model::Config config, std::shared_ptr<events::BroadcastChannel> events,
        model::Backend backend, agent::RunVersionLabels labels, agent::ToolClock clock, std::shared_ptr<RunLog> log = {});
    // 释放已完成的会话并按 Agent 约定保留实际未退出的工作。
    ~Subagents() override;
    // 为原插件创建一个独立上下文，允许在 DLL 启动时登记。
    runtime::DomainId open(const std::shared_ptr<plugins::PluginInstance>& instance) override;
    // 仅复制本轮输入，宿主在退出原 DLL 调用后发起模型请求。
    std::uint64_t submit(runtime::DomainId owner, runtime::DomainId session, SubagentInput input) override;
    // 按原所有者取得状态，答案视图保留至下次提交或关停。
    SubagentStatus status(runtime::DomainId owner, runtime::DomainId session) const override;
    // 按原请求取消，已完成的回执继续可查。
    bool cancel(runtime::DomainId owner, runtime::DomainId session, std::uint64_t request) override;
    // 请求关闭原上下文，实际退出后释放槽位。
    void close(runtime::DomainId owner, runtime::DomainId session) override;
    // 停止该插件的所有会话接纳并清除未派发输入。
    void close_owner(runtime::DomainId owner) noexcept override;
    // 当原插件仍有排队或在途回合时报告忙碌。
    bool busy(runtime::DomainId owner) const noexcept override;
    // 检查停机时全部子会话已实际退出。
    bool empty() const noexcept;
    // 停止接纳并请求关闭全部子会话。
    void stop() noexcept;
    // 对每个已登记子会话推进一次，处理开始、取消和实际退出。
    void advance();
  private:
    struct Slot;
    // 绑定原所有者及会话，拒绝过期或其他实例提交。
    Slot& find(runtime::DomainId owner, runtime::DomainId id) const;
    // 从原实例构造本轮工具权限、控制快照及独立运行。
    void begin(Slot& slot, const std::shared_ptr<plugins::PluginInstance>& instance);

    plugins::PluginManager& plugins_;
    model::Config config_;
    std::shared_ptr<events::BroadcastChannel> events_;
    model::Backend backend_;
    agent::RunVersionLabels labels_;
    agent::ToolClock clock_;
    std::shared_ptr<RunLog> log_;
    std::array<std::unique_ptr<Slot>,16> slots_;
    bool stopping_ = false;
};
}
