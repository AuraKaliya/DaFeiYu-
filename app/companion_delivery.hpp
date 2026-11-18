#pragma once
#include "companion_question.hpp"

namespace dfy::app {
enum class DeliveryPhase { pending, checking, accepted, stale, unavailable };
struct DeliveryStatus {
    std::uint64_t grant = 0;
    DeliveryPhase phase = DeliveryPhase::pending;
    std::size_t submissions = 0, checks = 0;
    std::string receipt;
};
// 当前运行内的有界委托投递，状态不推断游戏已开始开采。
class CompanionDelivery {
  public:
    // 按原授权投递或用只读查询恢复丢失回执。
    void advance(const std::optional<CollectionGrant>& grant, const std::optional<CompanionAddress>& address,
        const agent::ToolCatalog& catalog, const std::string& admit, const std::string& receipt, const agent::ToolClock& clock);
    // 取消本地在途操作，已接单事实仍可读取。
    void close();
    // 返回本地投递状态和可信原回执。
    const DeliveryStatus& status() const noexcept { return status_; }
    // 判断本地工具及原生票据是否尚待退出。
    bool busy() const noexcept { return static_cast<bool>(executor_); }
  private:
    // 收取一次真实工作结果并决定下一步只能回查还是允许原命令再投。
    void finish(const CollectionGrant& grant);
    DeliveryStatus status_;
    std::string args_;
    std::unique_ptr<agent::ToolExecutor> executor_;
    std::shared_ptr<agent::ToolRequest> work_;
    agent::ToolTime next_{};
    bool checking_ = false, closed_ = false;
};
}
