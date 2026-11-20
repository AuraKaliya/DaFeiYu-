#pragma once
#include "pipe_exchange.hpp"
#include "world_protocol.hpp"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <memory>

namespace dfy::mc {
// 插件直接订阅可信玩家事实，按世界代次独立保存游标与待处理事件。
class PartnerFeed {
  public:
    // 推进一个非等待管道步骤，首次绑定只交付当前在场事实。
    void advance(const BridgeConfig& config, std::uint64_t instance);
    // 停止读取并撤销待处理事实。
    void clear() noexcept;
    // 返回本轮可消费的原世界、玩家及事件；有续页时等到读完当前窗口。
    bool ready() const noexcept { return ready_ && !more_; }
    // 当前配对的游戏世界，失去绑定时为空。
    const std::string& world() const noexcept { return generation_; }
    // 当前世界经过私有读口验证的绑定玩家。
    const std::string& owner() const noexcept { return owner_; }
    // 返回收件确认的连续水位。
    std::uint64_t cursor() const noexcept { return cursor_; }
    // 返回本世界最近实际离场的序号，重新在场后归零。
    std::uint64_t departed() const noexcept { return departed_; }
    // 借用本次原生读取的世界和位置事实。
    const nlohmann::json& observation() const noexcept { return observation_; }
    // 借用待交付原始事件，按真实游戏次序排列。
    const nlohmann::json& pending() const noexcept { return pending_; }
    // 返回待交付事件之前是否丢失完整窗口。
    bool gap() const noexcept { return gap_; }
    // 清除已经交给原世界子会话的批次。
    void consumed();
  private:
    // 以已验证世界事实刷新来源，代次改变时撤销旧聊天。
    void accept_world(std::string_view response);
    // 从已验证的私有收件窗口更新待交付事件。
    void accept_inbox(std::string_view response);
    // 清空玩家绑定与游标，不重用旧世界事实。
    void reset_binding() noexcept;

    std::string generation_, owner_, id_, arguments_;
    nlohmann::json observation_;
    nlohmann::json pending_ = nlohmann::json::array();
    std::unique_ptr<PipeExchange> pipe_;
    std::chrono::steady_clock::time_point next_{}, world_due_{};
    std::uint64_t sequence_ = 0, cursor_ = 0, departed_ = 0;
    bool reading_world_ = false, ready_ = false, more_ = false, gap_ = false;
};
}
