#pragma once
#include <dafeiyu/plugin_manager.hpp>
#include <dafeiyu/plugin_tools.hpp>
#include <dafeiyu/plugin_agents.hpp>
#include <dafeiyu/session.hpp>
#include <array>

namespace dfy::app {
class RunLog;
class MessageStore;
struct PersonaProfile;
using plugins::SubagentInput;
using plugins::SubagentPhase;
using plugins::SubagentStatus;

// 宿主按插件代次持有独立会话，插件提交与模型或工具执行之间隔着推进边界。
class Subagents final : public plugins::PluginAgents {
  public:
    // 固定共享后端、通知、时钟及原插件目录。
    Subagents(plugins::PluginManager& plugins, model::Config config, std::shared_ptr<events::BroadcastChannel> events,
        model::Backend backend, agent::RunVersionLabels labels, agent::ToolClock clock, const PersonaProfile& persona,
        std::shared_ptr<RunLog> log = {});
    // 释放已完成的会话并按 Agent 约定保留实际未退出的工作。
    ~Subagents() override;
    // 为原插件创建一个独立上下文，允许在 DLL 启动时登记。
    runtime::DomainId open(const std::shared_ptr<plugins::PluginInstance>& instance) override;
    // 主档案绑定后才公开持久子会话服务。
    void bind_memory(std::shared_ptr<MessageStore> store);
    // 仅在本体已接续档案时允许协商记忆接口。
    bool memory_available() const noexcept override { return static_cast<bool>(memory_); }
    // 核对调用实例并把新运行号绑定到指定持久子库。
    runtime::DomainId open_bound(const std::shared_ptr<plugins::PluginInstance>& instance,const plugins::MemoryIdentity& who) override;
    // 将插件显式交付的记录写入当前绑定。
    std::uint64_t remember(runtime::DomainId owner,runtime::DomainId session,const std::string& event,
        const std::string& topic,const std::string& content) override;
    // 为后续请求选择获准的精确主题，换题时隔离旧历史。
    void select_topic(runtime::DomainId owner,runtime::DomainId session,std::string topic) override;
    // 选择个人或知识视图时隔离上一视图的历史。
    void select_knowledge(runtime::DomainId owner,runtime::DomainId session,std::string topic) override;
    // 发布状态或下游授权改变后停用已经见过该版的运行。
    void invalidate_knowledge(const std::string& article,std::int64_t child=0);
    // 停用授权已经变更的子会话，包含在途和已结算但尚可读取的旧答复。
    void invalidate_memory(std::int64_t child,std::string_view source={});
    // 新信落盘后通知实际信箱泵发送一个有界指针。
    void on_letter(std::function<void()> listener) { letter_ready_=std::move(listener); }
    // 登记或续接本委托的启动原信。
    std::uint64_t begin_letter(runtime::DomainId owner,runtime::DomainId session,std::string exchange,unsigned mode,bool persona) override;
    // 私下交付实际退出状态及插件选择导出的报告。
    std::uint64_t finish_letter(runtime::DomainId owner,runtime::DomainId session,std::string summary,std::string body,std::uint64_t note) override;
    // 仅复制本轮输入，宿主在退出原 DLL 调用后发起模型请求。
    std::uint64_t submit(runtime::DomainId owner, runtime::DomainId session, SubagentInput input) override;
    // 把回执写入原实例会话的工作记录，陈旧的授权不能发起新工作。
    std::uint64_t settle(runtime::DomainId owner, runtime::DomainId session, plugins::SubagentWorkFact fact) override;
    // 仅从原实例会话的指定代次读取已接纳的工作阶段。
    std::optional<std::string> read_receipt(runtime::DomainId owner, runtime::DomainId session,
        std::uint64_t epoch, std::uint64_t receipt) const override;
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
    struct WorkSlot;
    // 从原所有者和会话号查工作索引，退役索引只持有数据。
    WorkSlot* work_slot(runtime::DomainId owner, runtime::DomainId id) const noexcept;
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
    const PersonaProfile& persona_;
    std::shared_ptr<RunLog> log_;
    std::shared_ptr<MessageStore> memory_;
    std::function<void()> letter_ready_;
    std::array<std::unique_ptr<Slot>,16> slots_;
    std::array<std::unique_ptr<WorkSlot>,16> work_slots_;
    bool stopping_ = false;
};
}
