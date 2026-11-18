#pragma once
#include "companion_visit.hpp"
#include <dafeiyu/tool_executor.hpp>

namespace dfy::app {
enum class QuestionPhase { sending, waiting, declined, unavailable, withdrawn };
struct CompanionQuestion {
    CompanionAddress address;
    std::uint64_t id = 0, revision = 1, task = 0, order = 0, after = 0;
    std::string owner, text;
    QuestionPhase phase = QuestionPhase::sending;
};

// 一个发现结果最多开启一个玩家问题，通过原实例的工具确认游戏侧发送。
class CompanionQuestions {
  public:
    // 预留问题与原动作关联，提交实际插件发送并保存编号。
    void open(const CompanionVisit& visit, agent::ToolSnapshot scope, const agent::ToolClock& clock);
    // 取得已实际退出的发送回执，核对原玩家与提问水位。
    void advance();
    // 按原世界和玩家消息关闭问题，保留已完成的查看证据。
    bool decline(const CompanionAddress& address, std::string_view owner, std::uint64_t sequence, std::string_view question);
    // 换世界或停止时撤销待答状态并等发送工作退出。
    void withdraw();
    // 读取最近问题副本。
    const std::optional<CompanionQuestion>& current() const noexcept { return question_; }
    // 查询是否仍持有原生发送工作。
    bool busy() const noexcept { return static_cast<bool>(executor_); }
  private:
    std::optional<CompanionQuestion> question_;
    std::unique_ptr<agent::ToolExecutor> executor_;
    std::shared_ptr<agent::ToolRequest> work_;
};
}
