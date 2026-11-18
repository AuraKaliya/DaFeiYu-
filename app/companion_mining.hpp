#pragma once
#include "companion_delivery.hpp"

namespace dfy::app {
enum class MiningPhase { idle, submitted, client_finished, failed, withdrawn };
struct MiningStatus {
    std::uint64_t grant = 0, index = 0;
    MiningPhase phase = MiningPhase::idle;
    bool started = false;
    agent::ToolEffect effect = agent::ToolEffect::not_applied;
    std::string evidence;
    std::uint64_t lower = 0, upper = 0;
    std::vector<std::string> pickup_sources;
};
// 在游戏确认接单后驱动原授权的单块工作，入包证据由后续核验提供。
class CompanionMining {
  public:
    // 推进原工作或接纳已获准范围的第一块。
    void advance(const std::optional<CollectionGrant>& grant, const DeliveryStatus& delivery,
        const std::optional<CompanionAddress>& address, const agent::ToolCatalog& catalog, const std::string& tool,
        const agent::ToolClock& clock);
    // 截止当前委托的新工作，并等待实际原生退出。
    void close();
    // 返回当前块及已取得的客户端回执。
    const MiningStatus& status() const noexcept { return status_; }
    // 检查是否仍有原生票据待回收。
    bool busy() const noexcept { return static_cast<bool>(executor_); }
  private:
    // 在一个已退出的单块工作上结算至多一份自采来源。
    void credit(const std::string& evidence);
    MiningStatus status_;
    std::unique_ptr<agent::ToolExecutor> executor_;
    std::shared_ptr<agent::ToolRequest> work_;
    bool closed_ = false;
};
}
