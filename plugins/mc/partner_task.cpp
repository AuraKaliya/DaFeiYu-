#include "partner_task.hpp"
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
}

// 提供明确的当前目标、等待条件和最后一份由执行器报告的事实。
Json PartnerTask::context() const {
    Json value{{"revision",revision_},{"goal",goal_},{"detail",detail_},{"next",next_},{"continuationsLeft",remaining_}};
    if (!receipt_.is_null()) value["lastAction"] = receipt_;
    return value;
}

// 每次任务修改使用原版本，不以模型描述替代现场回执。
// 步骤：核对任务修订和正文 → 保存新的约定与观察基线 → 设置一次唤醒。
Json PartnerTask::update(const Json& input, const Json& scene, std::uint64_t scene_revision) {
    if (input.at("revision") != revision_ || revision_ == UINT64_MAX)
        return {{"status","stale"},{"task",context()}};
    auto goal = input.at("goal").get<std::string>();
    auto detail = input.value("detail",std::string{});
    auto next = input.at("next").get<std::string>();
    if (goal.empty() || goal.size() > 1024 || detail.size() > 2048 ||
        (next != "continue" && next != "scene" && next != "player" && next != "done"))
        throw std::invalid_argument("MC partner: invalid task update");
    auto baseline = next == "scene" ? scene_key(scene,scene_revision) : Json{};
    goal_ = std::move(goal);
    detail_ = std::move(detail);
    next_ = std::move(next);
    baseline_ = std::move(baseline);
    armed_ = true;
    ++revision_;
    return {{"status","updated"},{"task",context()}};
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
