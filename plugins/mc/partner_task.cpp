#include "partner_task.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace dfy::mc {
namespace {
using Json = nlohmann::json;
constexpr unsigned continuation_budget = 4;
constexpr std::size_t observation_limit = 32;

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

// 解析原话观察中的开口候选，仅使用游戏生成的范围。
// 步骤：查找候选 → 核对范围及轴向 → 展开为确切方格。
Json candidate_cells(const Json& observation, const Json& candidate) {
    const auto& gaps = observation.at("playerFocus").at("candidates");
    const auto choice = std::ranges::find_if(gaps,[&](const auto& gap) { return gap.at("id") == candidate; });
    if (choice == gaps.end()) return nullptr;
    const auto height = choice->at("height").get<int>();
    const auto width = choice->value("width",1);
    const auto axis = choice->at("axis").get<std::string>();
    if (height < 1 || height > 2 || width < 1 || width > 4 || (axis != "x" && axis != "z")) return nullptr;
    const auto x = choice->at("x").get<std::int64_t>();
    const auto y = choice->at("y").get<std::int64_t>();
    const auto z = choice->at("z").get<std::int64_t>();
    Json cells = Json::array();
    for (int dy = 0; dy < height; ++dy) for (int offset = 0; offset < width; ++offset)
        cells.push_back({{"x",x+(axis == "x" ? offset : 0)},{"y",y+dy},{"z",z+(axis == "z" ? offset : 0)}});
    return cells;
}

// 检查目标显式坐标的形状、范围和唯一性。
bool valid_cells(const Json& cells) {
    if (!cells.is_array() || cells.empty() || cells.size() > 16) return false;
    for (auto it = cells.begin(); it != cells.end(); ++it) {
        if (!it->is_object() || it->size() != 3 || std::find(cells.begin(),it,*it) != it) return false;
        for (const auto axis : {"x","y","z"})
            if (!it->contains(axis) || !it->at(axis).is_number_integer() || std::abs(it->at(axis).get<double>()) > 30000000)
                return false;
    }
    return true;
}
}

// 提供明确的当前目标、等待条件和最后一份由执行器报告的事实。
Json PartnerTask::context() const {
    Json value{{"revision",revision_},{"goal",goal_},{"detail",detail_},{"next",next_},{"continuationsLeft",remaining_},
        {"collection",collection_}};
    if (!receipt_.is_null()) value["lastAction"] = receipt_;
    if (!changes_.empty()) value["worldChanges"] = changes_;
    if (has_target()) {
        value["target"] = target_;
        const auto original = observations_.find(target_.at("message").get<std::uint64_t>());
        if (original != observations_.end()) value["targetObservation"] = original->second;
    }
    return value;
}

// 用插件持有的修订提交来自当前回合的任务意图。
// 步骤：校验正文与原话目标 → 保存约定和观察基线 → 分配新修订及唤醒条件。
Json PartnerTask::update(const Json& input, const Json& scene, std::uint64_t scene_revision, const Json* anchor,
    std::uint64_t player_order) {
    if (revision_ == UINT64_MAX) throw std::overflow_error("MC partner: task revision exhausted");
    auto goal = input.at("goal").get<std::string>();
    auto detail = input.value("detail",goal == goal_ ? detail_ : std::string{});
    auto next = input.at("next").get<std::string>();
    auto collection = input.value("collection",collection_);
    if (goal.empty() || goal.size() > 1024 || detail.size() > 2048 ||
        (collection != "visible" && collection != "ground") ||
        (next != "continue" && next != "scene" && next != "player" && next != "done"))
        throw std::invalid_argument("MC partner: invalid task update");
    auto baseline = next == "scene" ? scene_key(scene,scene_revision) : Json{};
    auto target = target_;
    if (input.contains("target")) {
        target = input.at("target");
        if (target.empty()) target = nullptr;
        else {
            const auto message = target.at("message").get<std::uint64_t>();
            const bool approach = target.value("approach",false);
            const bool existing = target_.contains("message") && target_.at("message") == message;
            if (!anchor || anchor->at("dimension") != scene.at("dimension") ||
                (anchor->at("position").is_null() && !approach))
                return {{"status","invalid_target"},{"reason","original player position unavailable"}};
            if (has_target() && !existing && (!target.value("replace",false) || player_order != message))
                return {{"status","invalid_target"},{"reason","new player instruction required to replace the target"}};
            if (approach && !(existing && target_.value("approach",false)) &&
                (player_order != message || target.contains("cells") || target.contains("candidate")))
                return {{"status","invalid_target"},{"reason","record the original approach before choosing cells"}};
            if (target.contains("candidate")) {
                if (target.contains("cells") || approach) return {{"status","invalid_target"},{"reason","choose an original candidate or cells"}};
                const auto original = observations_.find(message);
                auto cells = original == observations_.end() ? Json{} : candidate_cells(original->second,target.at("candidate"));
                if (cells.is_null()) return {{"status","invalid_target"},{"reason","candidate missing from original message observation"}};
                target["cells"] = std::move(cells);
            }
            if (target.contains("cells")) {
                const auto& cells = target.at("cells");
                if (!valid_cells(cells) || (approach && !verified_after_approach(target,scene)) ||
                    (!anchor->at("position").is_null() && !std::ranges::all_of(cells,[&](const auto& cell) {
                        return cell_near_anchor(cell,anchor->at("position")); })))
                    return {{"status","invalid_target"},{"reason","target cells lack current and original coverage"}};
                const auto previous = bound_cells_.find(message);
                if (previous != bound_cells_.end() && !std::ranges::is_permutation(previous->second,cells))
                    return {{"status","invalid_target"},{"reason","original target cells cannot be changed"}};
                bound_cells_[message] = cells;
                if (bound_cells_.size() > observation_limit) bound_cells_.erase(bound_cells_.begin());
            } else if (existing && target_.contains("cells")) {
                return {{"status","invalid_target"},{"reason","original target is already bound"}};
            }
            target.erase("replace");
            target["anchor"] = *anchor;
        }
    }
    goal_ = std::move(goal);
    detail_ = std::move(detail);
    next_ = std::move(next);
    collection_ = std::move(collection);
    baseline_ = std::move(baseline);
    target_ = std::move(target);
    armed_ = true;
    ++revision_;
    return {{"status","updated"},{"task",context()}};
}

// 允许原接近请求在实际到达后使用现场明确读回的空格。
// 步骤：核对工作来源和采样时间 → 验证角色在原到达点附近 → 核对每个空格。
bool PartnerTask::verified_after_approach(const Json& target, const Json& scene) const {
    if (arrival_.is_null() || local_observation_.is_null() || !target.contains("observation") ||
        arrival_.at("order") != target.at("message") || local_observation_.at("id") != target.at("observation") ||
        local_observation_.at("dimension") != scene.at("dimension") ||
        std::chrono::steady_clock::now()-observation_time_ > std::chrono::seconds(30)) return false;
    const auto& position = arrival_.at("position");
    const auto& observed = local_observation_.at("position");
    for (const auto axis : {"x","y","z"})
        if (std::abs(position.at(axis).get<double>()-observed.at(axis).get<double>()) > 3) return false;
    const auto& verified = local_observation_.at("verified");
    return std::ranges::all_of(target.at("cells"),[&](const auto& cell) {
        return std::ranges::any_of(verified,[&](const auto& sample) {
            return sample.at("atSelf") == true && sample.at("block") == "air" &&
                cell.at("x") == sample.at("x") && cell.at("y") == sample.at("y") && cell.at("z") == sample.at("z");
        });
    });
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
    if (target_.value("approach",false)) arguments["sourceMode"] = "approach";
    return true;
}

// 用任务范围约束所有采掘入口，包括精确坐标。
void PartnerTask::bind_collection(Json& arguments) const {
    if (collection_ == "ground") arguments["source"] = collection_;
}

// 分别保存原话指向、实际方块变化和最近一次动作。
// 步骤：归档有来源的观察 → 增补逐格变化 → 缩减网格并更新最近动作。
void PartnerTask::record(const Json& receipt) {
    if (receipt.at("skill") == "reply" || !receipt.contains("result")) return;
    auto summary = receipt;
    auto& result = summary.at("result");
    if (receipt.at("skill") == "move" && result.at("status") == "done" && result.value("target",std::string{}) == "player") {
        arrival_ = {{"order",receipt.at("order")},{"position",result.at("position")}};
        local_observation_ = nullptr;
    }
    if (receipt.at("skill") == "observe" && result.contains("anchor") && result.contains("playerFocus")) {
        const auto message = result.at("anchor").at("message").get<std::uint64_t>();
        observations_[message] = {{"message",message},{"sampledAtMs",result.at("sampledAtMs")},{"playerFocus",result.at("playerFocus")}};
        if (observations_.size() > observation_limit) observations_.erase(observations_.begin());
    }
    if (receipt.at("skill") == "observe" && result.contains("observation")) {
        auto& observation = result["observation"];
        auto& samples = observation["samples"];
        if (!samples.empty() && result.at("status") == "done" && samples.back().contains("verified")) {
            const auto& sample = samples.back();
            const auto& views = sample.at("views");
            const auto own = std::ranges::find_if(views,[](const auto& view) {
                return view.at("source") == "self" && view.at("status") == "sampled";
            });
            if (own != views.end()) {
                local_observation_ = {{"id",observation.at("id")},{"dimension",observation.at("dimension")},
                    {"position",own->at("actor").at("position")},{"verified",sample.at("verified")}};
                observation_time_ = std::chrono::steady_clock::now();
            }
        }
        for (auto& sample : samples) for (auto& view : sample["views"]) {
            if (view.erase("grid")) view["gridAvailable"] = true;
            if (!view.contains("playerFocus")) continue;
            const auto message = view.at("message").get<std::uint64_t>();
            observations_[message] = {{"message",message},{"sampledAtMs",sample.at("sampledAtMs")},
                {"playerFocus",view.at("playerFocus")}};
        }
        for (auto& sample : samples) sample.erase("palette");
        while (observations_.size() > observation_limit) observations_.erase(observations_.begin());
    }
    if ((receipt.at("skill") == "collect" || receipt.at("skill") == "place") && result.contains("changes")) {
        for (auto cell : result.at("changes")) {
            cell["work"] = receipt.at("bridgeWork");
            cell["order"] = receipt.at("order");
            changes_.push_back(std::move(cell));
        }
        if (changes_.size() > observation_limit)
            changes_.erase(changes_.begin(),changes_.end()-observation_limit);
    }
    if (result.is_object() && result.erase("survey")) result["surveyAvailable"] = true;
    if (summary.dump().size() <= 4096) receipt_ = std::move(summary);
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
