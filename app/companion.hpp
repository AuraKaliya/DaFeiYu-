#pragma once
#include "conversation.hpp"
#include "companion_state.hpp"
#include <dafeiyu/plugin_companion.hpp>
#include <dafeiyu/plugin_tools.hpp>

namespace dfy::app {
struct CompanionStatus {
    runtime::DomainId session = 0, instance = 0;
    std::string package, topic;
    bool stopping = false, closed = false;
    std::optional<agent::TurnDiagnostics> turn;
};

// 在应用宿主线程拥有同一玩家个体的一段游戏会话，工具绑定限原插件实例。
class CompanionSession {
  public:
    // 从活动实例建立独立会话、只读目录及可撤销角色声明。
    CompanionSession(const std::shared_ptr<plugins::PluginInstance>& instance, const model::Config& config,
        const std::shared_ptr<events::BroadcastChannel>& channel, const model::Backend& backend,
        const agent::RunVersionLabels& labels);
    // 接纳该会话的只读委托，插件重载时原会话拒绝新动作。
    agent::TurnDiagnostics submit(const std::string& prompt, const agent::ToolClock& clock);
    // 推进子会话的结算与实际退出，保留最近一次可信回执。
    void advance();
    // 撤销工具并请求关闭，仍需推进到已退出。
    void close();
    // 独立于对话回合，按需向原插件请求真实读回。
    std::uint64_t refresh(const agent::ToolClock& clock);
    // 复制已授权状态查询的结果及新旧标记。
    CompanionState state() const { return reader_.state(); }
    // 返回无对话正文的状态副本。
    CompanionStatus status() const;
    // 取得原会话身份，调用方用它指定停止或提问的目标。
    runtime::DomainId id() const noexcept { return conversation_.id(); }
    // 判断会话已停止接纳且所有在途工作已退出。
    bool closed() const noexcept { return conversation_.closed() && !reader_.state().pending; }
    // 读取仍保留的原回合结算值副本。
    std::optional<CompletedTurn> result(std::uint64_t turn) const;
  private:
    // 检查实例仍活动且属于原注册。
    bool bound() const noexcept;
    std::weak_ptr<plugins::PluginInstance> instance_;
    std::string package_;
    std::shared_ptr<agent::ToolCatalog> catalog_;
    plugins::PluginTools registration_;
    plugins::CompanionOffer offer_;
    agent::Session session_;
    Conversation conversation_;
    CompanionStateReader reader_;
    std::optional<CompletedTurn> result_;
    bool stopping_ = false;
};
}
