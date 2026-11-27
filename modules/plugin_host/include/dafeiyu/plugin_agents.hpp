#pragma once
#include <dafeiyu/runtime_ids.hpp>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace dfy::plugins {
class PluginInstance;
enum class MemoryPreset : unsigned { task=1, business, avatar };
struct MemoryIdentity { MemoryPreset preset; std::string subject, channel; };
enum class SubagentThinking { inherit, off, low };
struct SubagentFrame { std::uint64_t epoch, sequence; std::string scope; bool baseline; };
struct SubagentWorkFact {
    std::uint64_t epoch, revision;
    std::string work, content;
    std::uint32_t phase;
    // 可选有序接纳只在第一版填写，最终实效由领域所有者明确确认。
    std::uint64_t admission_sequence = 0;
    bool final_effect = false;
};
class SubagentGap : public std::runtime_error {
  public:
    // 携带原作用域下一序号供插件重送基线或补齐事实。
    explicit SubagentGap(std::uint64_t next) : std::runtime_error("subagent: fact sequence gap"),expected(next) {}
    std::uint64_t expected;
};
struct SubagentInput {
    std::string prompt, instructions, binding;
    std::vector<std::string> tools;
    bool allow_changes = false;
    bool replace_history = false;
    bool player_facing = false;
    SubagentThinking thinking = SubagentThinking::inherit;
    std::optional<SubagentFrame> frame;
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
    // 当前宿主是否已绑定支持持久身份的数据档案。
    virtual bool memory_available() const noexcept = 0;
    // 以真实插件实例和结构化业务身份打开独立会话。
    virtual runtime::DomainId open_bound(const std::shared_ptr<PluginInstance>& instance,const MemoryIdentity& identity) = 0;
    // 原实例向本会话所属的逻辑库幂等写入一项有来源的记录。
    virtual std::uint64_t remember(runtime::DomainId owner,runtime::DomainId session,const std::string& event,
        const std::string& topic,const std::string& content) = 0;
    // 为以后回合装配本会话获准的主题，业务忙碌时拒绝修改。
    virtual void select_topic(runtime::DomainId owner,runtime::DomainId session,std::string topic) = 0;
    // 接纳一个有界回合供后续宿主推进。
    virtual std::uint64_t submit(runtime::DomainId owner, runtime::DomainId session, SubagentInput input) = 0;
    // 只接纳同一实例及原工作号的回执，重复版本返回原接纳号。
    virtual std::uint64_t settle(runtime::DomainId owner, runtime::DomainId session, SubagentWorkFact fact) = 0;
    // 按调用者的原会话、代次和接纳号查询已保留的回执，读取不推进工作。
    virtual std::optional<std::string> read_receipt(runtime::DomainId owner, runtime::DomainId session,
        std::uint64_t epoch, std::uint64_t receipt) const = 0;
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
