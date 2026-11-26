#pragma once
#include "pipe_exchange.hpp"
#include "world_protocol.hpp"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <memory>
#include <deque>

namespace dfy::mc {
// 插件直接订阅可信玩家事实，按世界代次独立保存游标与待处理事件。
class PartnerFeed {
  public:
    // 推进一个非等待管道步骤，首次绑定只交付当前在场事实。
    void advance(const BridgeConfig& config, std::uint64_t instance);
    // 停止读取并撤销待处理事实。
    void clear() noexcept;
    // 返回本轮可消费的原世界、玩家及事件；有续页时等到读完当前窗口。
    bool ready() const noexcept { return readable() && !more_; }
    // 私有读口可用时仍能观察已释放或场景模式的游戏。
    bool readable() const noexcept { return ready_ && !recovering(); }
    // 短时读口故障期间保留已验证的配对和收件位置。
    bool recovering() const noexcept { return recover_until_ != std::chrono::steady_clock::time_point{}; }
    // 已核验的执行绑定仍用于归属和原工作结算。
    bool execution_bound() const noexcept { return mode_ == "partner" && execution_ == "ready"; }
    // 只在桥接明确拥有当前伙伴执行入口时允许接纳新动作。
    bool execution_ready() const noexcept { return readable() && execution_bound(); }
    // 返回旧绑定是否已被撤销的水位，同一游戏代次重新绑定也会推进。
    std::uint64_t binding_revision() const noexcept { return binding_revision_; }
    // 返回当前桥接执行模式，未报告时为空。
    const std::string& mode() const noexcept { return mode_; }
    // 返回当前执行入口状态，未报告的桥接无法驱动伙伴。
    const std::string& execution() const noexcept { return execution_; }
    // 当前配对的游戏世界，失去绑定时为空。
    const std::string& world() const noexcept { return generation_; }
    // 当前世界经过私有读口验证的绑定玩家。
    const std::string& owner() const noexcept { return owner_; }
    // 返回收件确认的连续水位。
    std::uint64_t cursor() const noexcept { return cursor_; }
    // 返回本世界最近实际离场的序号，重新在场后归零。
    std::uint64_t departed() const noexcept { return departed_; }
    // 返回仍需撤销旧工作的离场水位，进场事件不能提前清除它。
    std::uint64_t departure() const noexcept { return departure_; }
    // 归还旧在场周期的已结算事件，保留重新进场后的事实。
    void settle_departure();
    // 借用本次原生读取的世界和位置事实。
    const nlohmann::json& observation() const noexcept { return observation_; }
    // 返回最近一次已验收世界读回的原查询号、客户端覆盖范围与接收时间。
    const nlohmann::json& world_sample() const noexcept { return world_sample_; }
    // 当前世界收到的最新私有现场修订，零表示来源暂未提供。
    std::uint64_t scene_revision() const noexcept { return scene_revision_; }
    // 借用由原游戏主动提出的当前接管回执。
    const nlohmann::json& control() const noexcept { return control_; }
    // 借用待交付原始事件，按真实游戏次序排列。
    const nlohmann::json& pending() const noexcept { return pending_; }
    // 查询当前原话窗口中已验证的现场快照。
    const nlohmann::json* message_scene(std::uint64_t sequence) const noexcept;
    // 返回待交付事件之前是否丢失完整窗口。
    bool gap() const noexcept { return gap_; }
    // 返回本次在场尚未交给决策的空气边界；读取不会消费原玩家消息。
    const nlohmann::json& air_notices() const noexcept { return air_notices_; }
    // 仅在同场决策实际接纳后消费已展示的空气边界。
    void consume_air() noexcept { air_notices_.clear(); }
    // 清除已经交给原世界子会话的批次。
    void consumed();
  private:
    // 以已验证世界事实刷新来源，代次改变时撤销旧聊天。
    void accept_world(std::string_view response);
    // 从已验证的私有收件窗口更新待交付事件。
    void accept_inbox(std::string_view response);
    // 清空玩家绑定与游标，不重用旧世界事实。
    void reset_binding() noexcept;

    std::string generation_, owner_, id_, arguments_, mode_, execution_;
    nlohmann::json observation_, control_, world_sample_;
    nlohmann::json pending_ = nlohmann::json::array();
    nlohmann::json air_notices_ = nlohmann::json::array();
    std::deque<nlohmann::json> anchored_messages_;
    std::unique_ptr<PipeExchange> pipe_;
    std::chrono::steady_clock::time_point next_{}, world_due_{}, recover_until_{};
    std::uint64_t sequence_ = 0, cursor_ = 0, departed_ = 0, departure_ = 0, scene_revision_ = 0;
    std::uint64_t binding_revision_ = 0;
    std::uint64_t air_cursor_ = 0;
    bool air_requested_ = false, air_legacy_ = false, air_confirmed_ = false;
    bool reading_world_ = false, ready_ = false, more_ = false, gap_ = false;
};
}
