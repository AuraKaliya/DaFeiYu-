#pragma once
#include "pipe_exchange.hpp"
#include "world_protocol.hpp"
#include <memory>

namespace dfy::mc {
// 在原生工具票据内依次提交、查询、停止同一游戏动作，失联后不重发接纳。
class InspectWork {
  public:
    // 固定本次配对和已校验的玩家消息参数。
    InspectWork(const BridgeConfig& config, std::string id, std::string arguments);
    // 只在原生轮询时收发控制帧，返回有退出依据的最终回执。
    std::optional<InspectReply> poll(bool cancelled);
    // 查询是否可能已有动作接纳，用于错误时保守标记效果。
    bool attempted() const noexcept { return attempted_; }
  private:
    const BridgeConfig& config_;
    std::string id_, arguments_;
    std::unique_ptr<PipeExchange> pipe_;
    enum class Phase { begin, read, stop } phase_ = Phase::begin;
    bool attempted_ = false, stopping_ = false;
    std::chrono::steady_clock::time_point next_{}, deadline_;
};
}
