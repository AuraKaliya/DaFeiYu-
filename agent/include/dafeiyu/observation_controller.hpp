#pragma once
#include <dafeiyu/observation_trigger.hpp>
#include <dafeiyu/session.hpp>

namespace dfy::agent {
struct ObservationDispatch {
    TriggerDecision decision;
    std::shared_ptr<TurnRequest> request;
    std::optional<TriggerReceipt> receipt;
};
// 将获准采样交给已有 Session；宿主保持会话及采样集合存活，并继续原 Session 的推进和收尾。
class ObservationController {
  public:
    // 固定目标和规则，当前观察触发只允许文本及显式只读工具；状态变更留待行动前置复核。
    ObservationController(Session& session, ObservationInbox& inbox, ObservationRule rule, std::string goal, RunOptions options);
    // 停止本控制器并取消自己接纳的在途运行，不关闭会话或取消其他用户运行。
    ~ObservationController();
    // 禁止复制触发接纳和停止责任。
    ObservationController(const ObservationController&) = delete;
    // 禁止赋值或移动覆盖活动规则。
    ObservationController& operator=(const ObservationController&) = delete;
    // 显式开启，宿主策略必须同时允许观察来源。
    void enable();
    // 关闭未来触发并请求自己的原运行取消，实际退出仍由 Session 确认。
    void stop() noexcept;
    // 处理一个有界触发机会；用户输入待处理或会话忙碌时不消费观察修订，不自行推进 Session。
    ObservationDispatch poll(bool user_pending = false);
    // 返回原触发规则已经记账的运行数。
    std::size_t runs() const noexcept { return trigger_.runs(); }
    // 查询仍由会话或客户端保留的最近运行，时钟故障也不阻止读取原结果与退出状态。
    std::shared_ptr<TurnRequest> current() const noexcept { return active_.lock(); }
  private:
    Session& session_;
    ObservationInbox& inbox_;
    const RunOptions options_;
    const std::string goal_;
    ObservationTrigger trigger_;
    std::weak_ptr<TurnRequest> active_;
    std::weak_ptr<const ObservationRecord> active_observation_;
    bool polling_ = false;
};
}
