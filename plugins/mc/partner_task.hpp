#pragma once
#include <cstdint>
#include <string_view>
#include <nlohmann/json.hpp>

namespace dfy::mc {
// 保存当前配对世界内的共同目标和下一次唤醒条件。
class PartnerTask {
  public:
    // 返回任务及最近的独立现场回执，供下一回合使用。
    nlohmann::json context() const;
    // 按模型看到的原版本更新分工，接纳后返回当前版本和约定。
    nlohmann::json update(const nlohmann::json& arguments, const nlohmann::json& scene);
    // 归档原动作证据，以独立字段保留来源。
    void record(const nlohmann::json& receipt);
    // 返回当前到期的唤醒原因，只有新的玩家输入才能补充自动回合额度。
    std::string_view trigger(const nlohmann::json& scene) const;
    // 返回本次玩家窗口内是否还有自主回合额度。
    bool can_continue() const noexcept { return remaining_ != 0; }
    // 消费唤醒条件，每次自主提交扣除一轮额度，新玩家事件恢复额度。
    void submitted(bool new_game_event, bool autonomous);
    // 撤销在途唤醒，仍保留当前玩家已知任务，供后续原话处理。
    void disarm() noexcept { armed_ = false; }
  private:
    std::uint64_t revision_ = 0;
    std::string goal_, detail_, next_ = "player";
    unsigned remaining_ = 0;
    bool armed_ = false;
    nlohmann::json baseline_, receipt_;
};
}
