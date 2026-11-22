#pragma once
#include <cstdint>
#include <chrono>
#include <map>
#include <string_view>
#include <nlohmann/json.hpp>

namespace dfy::mc {
struct DigIntent {
    bool pointed = false;
    std::string_view material, support;
};
// 从玩家原话识别可确定的指向、材料和上下关系。
DigIntent read_dig_intent(std::string_view text);

// 保存当前配对世界内的共同目标和下一次唤醒条件。
class PartnerTask {
  public:
    // 返回任务及最近的独立现场回执，供下一回合使用。
    nlohmann::json context() const;
    // 返回执行修订，供计划、待执行动作和确认核对原目标与范围。
    std::uint64_t revision() const noexcept { return revision_; }
    // 返回新任务的原始玩家消息号及当前执行指令。
    std::uint64_t id() const noexcept { return id_; }
    std::uint64_t instruction() const noexcept { return instruction_; }
    // 判断当前任务是否使用导航目标。
    bool navigation() const noexcept { return kind_ == "navigation"; }
    // 只把当前任务已核验的路线及检查点送到游戏侧。
    bool bind_route(nlohmann::json& arguments) const;
    // 保存已接纳路线的墙体、原观察和圈数，供同一任务重开。
    void accept_route(const nlohmann::json& arguments);
    // 收尾有限路线，随后只由新原话继续任务。
    void finish_route(bool completed);
    // 登记新任务或重开原任务，保留可供验证的旧对象线索。
    nlohmann::json switch_task(const nlohmann::json& arguments, const nlohmann::json& event);
    // 返回原纠正仍未核实退出的工作号。
    std::string_view unconfirmed() const noexcept { return unconfirmed_work_; }
    // 返回当前共同目标说明。
    std::string_view goal() const noexcept { return goal_; }
    // 返回当前取材约束，供局内任务更新识别范围变化。
    std::string_view collection() const noexcept { return collection_; }
    // 更新当前分工，说明或唤醒方式变化沿用原执行修订。
    nlohmann::json update(const nlohmann::json& arguments, const nlohmann::json& scene, std::uint64_t scene_revision,
        const nlohmann::json* anchor, std::uint64_t player_order = 0);
    // 将当前原话登记为待完成的任务纠正。
    nlohmann::json begin_correction(const nlohmann::json& event, const nlohmann::json& scene);
    // 保存无法证实已退出的旧工作身份，暂停这次世界绑定中的纠正提交。
    void hold_correction(std::string work);
    // 判断旧目标是否正在等待玩家纠正落地。
    bool correcting() const noexcept { return !correction_.is_null(); }
    // 判断当前任务是否带有需要遵循的原话目标。
    bool has_target() const noexcept { return !target_.is_null(); }
    // 只有目标已绑定确切方块时才自动授予放置目录。
    bool target_ready() const noexcept { return !correcting() && target_.contains("cells"); }
    // 仅允许目标内的确切方块，并把来源和任务修订固定进原动作。
    bool bind_placement(nlohmann::json& arguments) const;
    // 以固定顺序返回当前目标的全部方格。
    const nlohmann::json& target_cells() const { return target_.at("cells"); }
    // 将当前任务的取材范围固定到实际采掘参数中。
    void bind_collection(nlohmann::json& arguments) const;
    // 更新最近动作，按原消息号独立保存指向证据。
    void record(const nlohmann::json& receipt, bool current = true);
    // 比对原话和可信观察，存在参照关系时把它传给执行器重验。
    bool bind_dig(std::uint64_t message, const DigIntent& intent, nlohmann::json& arguments) const;
    // 返回当前到期的唤醒原因，只有新的玩家输入才能补充自动回合额度。
    std::string_view trigger(const nlohmann::json& scene, std::uint64_t scene_revision) const;
    // 返回本次玩家窗口内是否还有自主回合额度。
    bool can_continue() const noexcept { return remaining_ != 0; }
    // 消费唤醒条件，每次自主提交扣除一轮额度，新玩家事件恢复额度。
    void submitted(bool new_game_event, bool autonomous);
    // 撤销在途唤醒，仍保留当前玩家已知任务，供后续原话处理。
    void disarm() noexcept { armed_ = false; }
  private:
    // 保存最新命中并保留原话首次确切的墙面指向。
    void record_focus(std::uint64_t message, const nlohmann::json& sampled_at, const nlohmann::json& focus);
    // 核对接近原请求后的现场证据仍属于同一任务和维度。
    bool verified_after_approach(const nlohmann::json& target, const nlohmann::json& scene) const;
    // 验证近期自身视图已覆盖全部拟切换的方格。
    bool verified_cells(const nlohmann::json& target, const nlohmann::json& scene) const;
    std::uint64_t revision_ = 0, id_ = 0, instruction_ = 0;
    std::uint64_t last_correction_ = 0;
    std::string kind_, goal_, detail_, next_ = "player", collection_ = "visible";
    std::string unconfirmed_work_;
    unsigned remaining_ = 0;
    bool armed_ = false, route_committed_ = false;
    nlohmann::json baseline_, receipt_, target_, correction_, previous_target_, changes_ = nlohmann::json::array();
    std::map<std::uint64_t,nlohmann::json> observations_;
    std::map<std::uint64_t,nlohmann::json> object_observations_;
    std::map<std::uint64_t,nlohmann::json> openings_;
    std::map<std::uint64_t,nlohmann::json> bound_cells_;
    nlohmann::json arrival_, local_observation_, route_, navigation_wall_;
    std::chrono::steady_clock::time_point observation_time_{};
};
}
