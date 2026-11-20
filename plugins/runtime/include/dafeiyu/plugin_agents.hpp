#pragma once
#include <dafeiyu/runtime_ids.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dfy::plugins {
class PluginInstance;
struct SubagentInput {
    std::string prompt, instructions, binding;
    std::vector<std::string> tools;
    bool allow_changes = false;
};
enum class SubagentPhase { idle, queued, running, succeeded, failed, cancelled };
struct SubagentStatus {
    std::uint64_t request = 0;
    SubagentPhase phase = SubagentPhase::idle;
    std::string_view answer;
};

// 运行时只借用子会话服务，具体 Agent 与模型由应用层装配。
class PluginAgents {
  public:
    // 由实际实现管理会话退出及资源寿命。
    virtual ~PluginAgents() = default;
    // 为宿主绑定的原实例创建独立上下文。
    virtual runtime::DomainId open(const std::shared_ptr<PluginInstance>& instance) = 0;
    // 接纳一个有界回合供后续宿主推进。
    virtual std::uint64_t submit(runtime::DomainId owner, runtime::DomainId session, SubagentInput input) = 0;
    // 取得原会话最新请求；正文在下一次修改前有效。
    virtual SubagentStatus status(runtime::DomainId owner, runtime::DomainId session) const = 0;
    // 请求取消指定回合并保留其结果身份。
    virtual bool cancel(runtime::DomainId owner, runtime::DomainId session, std::uint64_t request) = 0;
    // 请求关闭一个子会话并由宿主完成退出。
    virtual void close(runtime::DomainId owner, runtime::DomainId session) = 0;
    // 同时关闭原实例所有子会话，未开始的工作可以立即撤销。
    virtual void close_owner(runtime::DomainId owner) noexcept = 0;
    // 报告原实例尚有排队或未退出工作，卸载应明确拒绝。
    virtual bool busy(runtime::DomainId owner) const noexcept = 0;
};
}
