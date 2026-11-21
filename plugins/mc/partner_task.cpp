#include "partner_task.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace dfy::mc {
namespace {
using Json = nlohmann::json;
constexpr unsigned continuation_budget = 4;

// 提取现场变化修订与实际地点，忽略采样时间和小幅位置抖动。
Json scene_key(const Json& scene, std::uint64_t revision) {
    if (!scene.is_object()) return nullptr;
    const auto& position = scene.at("position");
    return {scene.at("dimension"),std::floor(position.at("x").get<double>()),std::floor(position.at("y").get<double>()),
        std::floor(position.at("z").get<double>()),scene.at("cobblestone"),revision};
}

// 核对选定的方块在原话能够扫描的坐标窗口内。
bool cell_near_anchor(const Json& cell, const Json& position) {
    if (!cell.is_object() || cell.size() != 3) return false;
    for (const auto* axis : {"x","y","z"}) {
        if (!cell.at(axis).is_number_integer()) return false;
        const auto offset = cell.at(axis).get<double>()-std::floor(position.at(axis).get<double>());
        if (axis == std::string_view("y") ? offset < -4 || offset > 2 : std::abs(offset) > 5) return false;
    }
    return true;
}
}

// 提供明确的当前目标、等待条件和最后一份由执行器报告的事实。
Json PartnerTask::context() const {
    Json value{{"revision",revision_},{"goal",goal_},{"detail",detail_},{"next",next_},{"continuationsLeft",remaining_}};
    if (!receipt_.is_null()) value["lastAction"] = receipt_;
    if (has_target()) value["target"] = target_;
    return value;
}

// 用插件持有的修订提交来自当前回合的任务意图。
// 步骤：校验正文与原话目标 → 保存约定和观察基线 → 分配新修订及唤醒条件。
Json PartnerTask::update(const Json& input, const Json& scene, std::uint64_t scene_revision, const Json* anchor) {
    if (revision_ == UINT64_MAX) throw std::overflow_error("MC partner: task revision exhausted");
    auto goal = input.at("goal").get<std::string>();
    auto detail = input.value("detail",std::string{});
    auto next = input.at("next").get<std::string>();
    if (goal.empty() || goal.size() > 1024 || detail.size() > 2048 ||
        (next != "continue" && next != "scene" && next != "player" && next != "done"))
        throw std::invalid_argument("MC partner: invalid task update");
    auto baseline = next == "scene" ? scene_key(scene,scene_revision) : Json{};
    auto target = target_;
    if (input.contains("target")) {
        target = input.at("target");
        if (target.empty()) target = nullptr;
        else {
            if (!anchor || anchor->at("position").is_null() || anchor->at("dimension") != scene.at("dimension"))
                return {{"status","invalid_target"},{"reason","original player position unavailable"}};
            if (target.contains("cells")) {
                const auto& cells = target.at("cells");
                if (!cells.is_array() || cells.empty() || cells.size() > 16 ||
                    !std::ranges::all_of(cells,[&](const auto& cell) { return cell_near_anchor(cell,anchor->at("position")); }))
                    return {{"status","invalid_target"},{"reason","target cells outside the original survey"}};
            }
            target["anchor"] = *anchor;
        }
    }
    goal_ = std::move(goal);
    detail_ = std::move(detail);
    next_ = std::move(next);
    baseline_ = std::move(baseline);
    target_ = std::move(target);
    armed_ = true;
    ++revision_;
    return {{"status","updated"},{"task",context()}};
}

// 将实际写入的每个方块与当前任务选择核对，再固定原话和任务修订。
// 步骤：读取所需格 → 逐格检查是否属于原目标 → 绑定实际动作。
bool PartnerTask::bind_placement(Json& arguments) const {
    if (!target_.contains("cells")) return false;
    const auto& y = arguments.at("y");
    if (!y.is_number_integer() || std::abs(y.get<double>()) > 30000000) return false;
    const auto& cells = target_.at("cells");
    const auto& material = arguments.at("block").get_ref<const std::string&>();
    const int count = material.ends_with("_door") ? 2 : 1;
    for (int index = 0; index < count; ++index) {
        const Json cell{{"x",arguments.at("x")},{"y",y.get<std::int64_t>()+index},{"z",arguments.at("z")}};
        if (std::ranges::find(cells,cell) == cells.end()) return false;
    }
    arguments["sourceMessage"] = target_.at("message");
    arguments["taskRevision"] = revision_;
    return true;
}

// 记录最近一次有现场内容的工作，发言与任务更新无法冒充现场证据。
void PartnerTask::record(const Json& receipt) {
    if (receipt.at("skill") != "reply" && receipt.contains("result") && receipt.dump().size() <= 4096)
        receipt_ = receipt;
}

// 只在任务明确要求继续或收到所等观察变化时唤醒，单次额度耗尽后等待新玩家事件。
std::string_view PartnerTask::trigger(const Json& scene, std::uint64_t scene_revision) const {
    if (!armed_ || !remaining_) return {};
    if (next_ == "continue") return "task.continue";
    if (next_ == "scene" && !baseline_.is_null() && scene_key(scene,scene_revision) != baseline_) return "scene.changed";
    return {};
}

// 真实接纳后消费原唤醒，新玩家事件提供新的有限自主窗口。
void PartnerTask::submitted(bool new_game_event, bool autonomous) {
    if (new_game_event) remaining_ = continuation_budget;
    if (autonomous) --remaining_;
    disarm();
}
}
