#pragma once
#include "pipe_exchange.hpp"
#include "world_protocol.hpp"
#include <functional>
#include <memory>

namespace dfy::mc {
enum class GameAction { inspect, mine, home, partner };
enum class GameEffect { none, observed, unknown };
struct GameResult { bool success; GameEffect effect; std::string payload; bool accepted = false; };
struct GameAdmission { std::uint32_t status; std::optional<GameResult> blocked; };
// 在原生工具票据内依次提交、查询、停止同一游戏动作，失联后不重发接纳。
class GameWork {
  public:
    // 固定本次配对和已校验的玩家消息参数。
    GameWork(GameAction action, const BridgeConfig& config, std::string id, std::string arguments, std::string skill = {});
    // 推进原工作，暂缓接纳时仍能查询或停止已提交的工作。
    std::optional<GameResult> poll(bool cancelled, bool defer = false, bool allow_begin = true,
        const std::function<GameAdmission()>& before_begin = {});
    // 有界地重查同一工作退出，沿用原编号和幂等停止操作。
    void recheck_exit() noexcept;
    // 本地票据收回后只读原伙伴工作的回执，未能核实时等待下次读回。
    std::optional<GameResult> read_exit();
    // 查询是否可能已有动作接纳，用于错误时保守标记效果。
    bool attempted() const noexcept { return attempted_; }
    // 读取原桥接身份和实际技能以便登记宿主工作。
    const std::string& id() const noexcept { return id_; }
    // 返回已经校验且用于桥接协议的技能名称。
    const std::string& skill() const noexcept { return skill_; }
    // 未进入登记边界时为空，否则保留原号的接纳或拒绝状态。
    std::optional<std::uint32_t> fact_admission() const noexcept { return admission_ ? std::optional(admission_->status) : std::nullopt; }
    // 只有经过原工作身份和终态校验的桥接回执才能确认退出。
    bool exited() const noexcept { return exited_; }
    // 借出首次下发之前要复核的原动作参数。
    const std::string& arguments() const noexcept { return arguments_; }
    // 读取最近一次已核验的在途进展。
    const std::string& progress() const noexcept { return progress_; }
  private:
    const BridgeConfig config_;
    GameAction action_;
    std::string id_, arguments_, skill_, progress_;
    std::unique_ptr<PipeExchange> pipe_;
    enum class Phase { begin, read, stop, verify_exit } phase_ = Phase::begin;
    bool attempted_ = false, stopping_ = false, exited_ = false;
    std::optional<GameAdmission> admission_;
    unsigned transport_failures_ = 0;
    std::chrono::steady_clock::time_point next_{}, deadline_, stop_deadline_{};
};
}
