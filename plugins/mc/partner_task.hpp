#pragma once
#include <cstdint>
#include <chrono>
#include <map>
#include <string_view>
#include <nlohmann/json.hpp>

namespace dfy::mc {
// 保存当前配对世界内的共同目标和下一次唤醒条件。
class PartnerTask {
  public:
    // 返回任务及最近的独立现场回执，供下一回合使用。
    nlohmann::json context() const;
    // 返回当前任务修订，供待确认动作核对原目标。
    std::uint64_t revision() const noexcept { return revision_; }
    // 更新来自当前回合的分工，由插件分配并返回新修订。
    nlohmann::json update(const nlohmann::json& arguments, const nlohmann::json& scene, std::uint64_t scene_revision,
        const nlohmann::json* anchor, std::uint64_t player_order = 0);
    // 判断当前任务是否带有需要遵循的原话目标。
    bool has_target() const noexcept { return !target_.is_null(); }
    // 只有目标已绑定确切方块时才自动授予放置目录。
    bool target_ready() const noexcept { return target_.contains("cells"); }
    // 仅允许目标内的确切方块，并把来源和任务修订固定进原动作。
    bool bind_placement(nlohmann::json& arguments) const;
    // 以固定顺序返回当前目标的全部方格。
    const nlohmann::json& target_cells() const { return target_.at("cells"); }
    // 将当前任务的取材范围固定到实际采掘参数中。
    void bind_collection(nlohmann::json& arguments) const;
    // 更新最近动作，按原消息号独立保存指向证据。
    void record(const nlohmann::json& receipt);
    // 返回当前到期的唤醒原因，只有新的玩家输入才能补充自动回合额度。
    std::string_view trigger(const nlohmann::json& scene, std::uint64_t scene_revision) const;
    // 返回本次玩家窗口内是否还有自主回合额度。
    bool can_continue() const noexcept { return remaining_ != 0; }
    // 消费唤醒条件，每次自主提交扣除一轮额度，新玩家事件恢复额度。
    void submitted(bool new_game_event, bool autonomous);
    // 撤销在途唤醒，仍保留当前玩家已知任务，供后续原话处理。
    void disarm() noexcept { armed_ = false; }
  private:
    // 核对接近原请求后的现场证据仍属于同一任务和维度。
    bool verified_after_approach(const nlohmann::json& target, const nlohmann::json& scene) const;
    std::uint64_t revision_ = 0;
    std::string goal_, detail_, next_ = "player", collection_ = "visible";
    unsigned remaining_ = 0;
    bool armed_ = false;
    nlohmann::json baseline_, receipt_, target_, changes_ = nlohmann::json::array();
    std::map<std::uint64_t,nlohmann::json> observations_;
    std::map<std::uint64_t,nlohmann::json> bound_cells_;
    nlohmann::json arrival_, local_observation_;
    std::chrono::steady_clock::time_point observation_time_{};
};
}
