#pragma once
#include "companion_messages.hpp"
#include <dafeiyu/tool_executor.hpp>

namespace dfy::app {
struct PlayerUtterance {
    std::uint64_t sequence;
    std::string text, visit_place, declined_question, collection_question;
    std::uint64_t quantity = 0;
};
struct PlayerInboxStatus {
    bool enabled = false, ready = false, gap = false;
    std::uint64_t cursor = 0;
    std::string owner;
};

// 只接受由当前插件真实回执提供的玩家原话，初次绑定时从当前水位开始。
class CompanionInbox {
  public:
    // 选定插件登记的私有收件入口。
    void enable(std::string tool);
    // 切换实际游戏来源，撤销在途读取和旧待处理原话。
    void bind(std::optional<CompanionAddress> address);
    // 有界推进读取，按已确认游标收取新原话，不触发模型。
    void advance(const agent::ToolCatalog& catalog, const agent::ToolClock& clock);
    // 查询可信收件口的当前状态。
    const PlayerInboxStatus& status() const noexcept { return status_; }
    // 消费一条本代次已经验证的玩家原话。
    std::optional<PlayerUtterance> take();
    // 查询是否仍持有未退出的原生读取。
    bool busy() const noexcept { return static_cast<bool>(executor_); }
  private:
    // 校验来源和游标，只在整批成功后提交新收件水位。
    void accept(const std::string& payload);
    std::string tool_;
    std::optional<CompanionAddress> address_, reading_;
    PlayerInboxStatus status_;
    std::deque<PlayerUtterance> entries_;
    agent::ToolTime next_{};
    std::unique_ptr<agent::ToolExecutor> executor_;
    std::shared_ptr<agent::ToolRequest> work_;
};
}
