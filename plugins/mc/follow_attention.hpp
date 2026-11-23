#pragma once
#include <nlohmann/json.hpp>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace dfy::mc {
enum class FollowSignal { none, raised, recovered };

// 只用有时间戳的游戏读回判断哪些跟随变化需要新的决策。
class FollowAttention {
  public:
    // 用相邻样本维护注意窗口，同一异常只产生一次唤醒。
    // 步骤：核对采样时间 → 更新距离、高差和可见性窗口 → 检查受阻或持续恢复。
    FollowSignal observe(const nlohmann::json& progress, double range) {
        if (!progress.contains("sampledAtMs")) return FollowSignal::none;
        const auto at = progress.at("sampledAtMs").get<std::uint64_t>();
        if (last_ && at <= *last_) return FollowSignal::none;
        if (last_ && at-*last_ > 750) distant_ = high_ = missing_ = near_ = std::nullopt;
        last_ = at;
        evidence_ = progress;

        const auto phase = progress.at("phase").get_ref<const std::string&>();
        const auto distance = progress.value("distance",0.0);
        if (distance <= range+2) distant_.reset();
        else if (!distant_ || distance+1 <= baseline_) { distant_ = at; baseline_ = distance; }
        const auto path = progress.value("path","");
        const bool height_known = progress.contains("vertical");
        const bool close_height = height_known && std::abs(progress.at("vertical").get<double>()) < 2;
        const auto high = sustain(high_,height_known && !close_height && path == "unreachable",at,2000);
        const auto missing = sustain(missing_,phase == "waiting_player",at,5000);
        const auto near = sustain(near_,phase == "in_range" && distance <= range+0.5 && close_height && path == "reachable",at,1000);
        if (active_) {
            if (!near) return FollowSignal::none;
            active_ = false;
            return FollowSignal::recovered;
        }
        if (phase == "blocked") return raise("movement_blocked");
        if (missing) return raise("player_out_of_view");
        if (high) return raise("unreachable_height");
        if (distant_ && at-*distant_ >= 3000) return raise("distance_stalled");
        return FollowSignal::none;
    }

    // 已接纳工作实际受阻退出时立即记录，沿用已打开的异常。
    FollowSignal blocked(const nlohmann::json& result) {
        if (result.contains("progress")) evidence_ = result.at("progress");
        near_.reset();
        return raise("movement_blocked");
    }

    // 读取逻辑跟随的异常编号。
    std::uint64_t episode() const noexcept { return episode_; }
    // 判断原异常是否仍需要处理。
    bool active() const noexcept { return active_; }
    // 读取本次唤醒的原因。
    std::string_view reason() const noexcept { return reason_; }
    // 读取最近的真实跟随样本。
    const nlohmann::json& evidence() const noexcept { return evidence_; }

  private:
    // 记录条件首次成立的时间，不连续的条件重新计时。
    static bool sustain(std::optional<std::uint64_t>& since, bool condition, std::uint64_t at, std::uint64_t duration) {
        if (!condition) since.reset();
        else if (!since) since = at;
        return since && at-*since >= duration;
    }

    // 为尚未报告的异常分配一个编号。
    FollowSignal raise(std::string_view reason) {
        if (active_) return FollowSignal::none;
        active_ = true;
        reason_ = reason;
        ++episode_;
        return FollowSignal::raised;
    }

    std::optional<std::uint64_t> last_, distant_, high_, missing_, near_;
    double baseline_ = 0;
    std::uint64_t episode_ = 0;
    bool active_ = false;
    std::string_view reason_;
    nlohmann::json evidence_;
};
}
