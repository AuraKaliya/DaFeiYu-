#include "partner_task.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace dfy::mc {
namespace {
using Json = nlohmann::json;
constexpr unsigned continuation_budget = 4;
constexpr std::size_t observation_limit = 32;
using namespace std::string_view_literals;
constexpr std::array materials{
    std::pair{"草方块"sv,"grass_block"sv}, std::pair{"泥土"sv,"dirt"sv}, std::pair{"圆石"sv,"cobblestone"sv},
    std::pair{"石块"sv,"stone"sv}, std::pair{"石头"sv,"stone"sv}, std::pair{"橡木原木"sv,"oak_log"sv},
    std::pair{"原木"sv,"log"sv}, std::pair{"沙子"sv,"sand"sv}, std::pair{"沙砾"sv,"gravel"sv}
};

// 返回原话首先点名的可明确映射到游戏材料的名词。
std::string_view named_material(std::string_view text) {
    std::string_view result;
    std::size_t best = text.npos;
    for (const auto& [word,material] : materials) {
        if (const auto at = text.find(word); at < best) { best = at; result = material; }
    }
    return result;
}

// 比较游戏材质名，原木类别可对应实际观察到的种类。
bool same_material(std::string_view block, std::string_view material) {
    if (block.starts_with("minecraft:")) block.remove_prefix(10);
    if (material == "log") return block.ends_with("_log") || block.ends_with("_stem");
    return material.empty() || block == material;
}

// 比较两个来源是否指向同一格。
bool same_cell(const Json& left, const Json& right) {
    return std::ranges::all_of(std::array{"x","y","z"},[&](auto axis) { return left.at(axis) == right.at(axis); });
}

// 比较候选与实际动作所用的坐标和方块。
bool same_block(const Json& candidate, const Json& args) {
    return candidate.is_object() && same_material(args.at("block").get_ref<const std::string&>(),
        candidate.at("block").get_ref<const std::string&>()) && same_cell(candidate,args);
}

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

// 比较真正会改变执行范围的目标字段，候选说明和同一组格子的顺序可补记。
bool same_target_scope(const Json& a, const Json& b) {
    if (a == b) return true;
    if (a.is_null() || b.is_null() || a.at("message") != b.at("message") ||
        a.at("anchor").at("dimension") != b.at("anchor").at("dimension") ||
        a.value("approach",false) != b.value("approach",false) || a.contains("cells") != b.contains("cells")) return false;
    return !a.contains("cells") || std::ranges::is_permutation(a.at("cells"),b.at("cells"));
}
}

// 从原话提取破坏目标的明确约束，参照材料按原词序与动作目标分开。
// 步骤：按分句识别破坏动词 → 查找上下关系 → 核对当前分句或代词指向的材质。
DigIntent read_dig_intent(std::string_view text) {
    using namespace std::string_view_literals;
    constexpr std::array verbs{"挖"sv,"敲"sv,"拆"sv,"铲"sv,"砸"sv,"打掉"sv,"移除"sv};
    constexpr std::array pointing{"这块"sv,"这个"sv,"这格"sv,"这根"sv,"这里"sv,"我面前"sv,"眼前"sv,"掉它"sv};
    std::string_view previous;
    bool gathering_hint = false;
    while (!text.empty()) {
        std::size_t end = text.size(), separator = 0;
        for (const auto delimiter : {"，"sv,"。"sv,"；"sv,"！"sv,"？"sv,","sv,";"sv,"!"sv,"?"sv}) {
            if (const auto at = text.find(delimiter); at < end) { end = at; separator = delimiter.size(); }
        }
        const auto clause = text.substr(0,end);
        const auto has_word = [&](auto word) { return clause.find(word) != clause.npos; };
        if (std::ranges::any_of(verbs,has_word)) {
            const auto material = named_material(clause);
            gathering_hint = true;
            for (const auto relation : {"上方的"sv,"上面的"sv,"上边的"sv,"上的"sv,"上面那块"sv}) {
                const auto at = clause.find(relation);
                if (at == clause.npos) continue;
                const auto support = named_material(clause.substr(at > 18 ? at-18 : 0,std::min<std::size_t>(at,18)));
                const auto target = named_material(clause.substr(at+relation.size(),24));
                if (!support.empty() && !target.empty()) return {true,target,support,true};
            }
            if (std::ranges::any_of(pointing,has_word)) {
                auto target = material;
                if (target.empty() && has_word("掉它")) target = named_material(previous);
                return {true,target,{},true};
            }
        }
        text.remove_prefix(end+separator);
        previous = clause;
    }
    return {false,{},{},gathering_hint};
}

// 提供明确的当前目标、等待条件和最后一份由执行器报告的事实。
Json PartnerTask::context() const {
    Json value{{"revision",revision_},{"goal",goal_},{"detail",detail_},{"next",next_},{"continuationsLeft",remaining_},
        {"collection",collection_}};
    if (id_) {
        value["id"] = id_;
        value["kind"] = kind_;
        value["instruction"] = instruction_;
    }
    if (!previous_target_.is_null()) value["previousTarget"] = previous_target_;
    if (!navigation_wall_.is_null()) value["navigationWall"] = navigation_wall_;
    if (!receipt_.is_null()) value["lastAction"] = receipt_;
    if (correcting()) {
        value["pendingCorrection"] = correction_;
        if (!unconfirmed_work_.empty()) value["pendingCorrection"]["unconfirmedWork"] = unconfirmed_work_;
    }
    if (!changes_.empty()) value["worldChanges"] = changes_;
    if (has_target()) {
        value["target"] = target_;
        const auto message = target_.at("message").get<std::uint64_t>();
        const auto& source = target_.contains("candidate") ? openings_ : observations_;
        const auto original = source.find(message);
        if (original != source.end()) value["targetObservation"] = original->second;
    }
    return value;
}

// 交接任务身份和执行修订，已完成对象继续作为观察线索。
// 步骤：核对玩家消息及旧任务身份 → 更新执行修订 → 清理前一目标的运行状态。
Json PartnerTask::switch_task(const Json& args, const Json& event) {
    const auto order = event.at("sequence").get<std::uint64_t>();
    const bool restart = args.at("operation") == "restart_task";
    const auto kind = restart ? kind_ : args.value("kind",std::string{});
    const auto goal = args.value("goal",goal_);
    if (order == instruction_ && id_ && (!restart || args.value("task",std::uint64_t{}) == id_))
        return {{"status","unchanged"},{"task",context()}};
    if (order < instruction_ || (restart && (!id_ || args.value("task",std::uint64_t{}) != id_)))
        return {{"status","stale"}};
    if ((kind != "navigation" && kind != "work") || goal.empty() || goal.size() > 1024 ||
        (restart ? goal != goal_ : !args.contains("goal")))
        return {{"status","invalid_target"},{"reason","invalid_task_intent"}};
    if (!unconfirmed_work_.empty()) return {{"status","old_work_exit_unconfirmed"},{"work",unconfirmed_work_}};
    if (revision_ == UINT64_MAX) throw std::overflow_error("MC partner: task revision exhausted");

    if (!restart) {
        id_ = order;
        if (has_target()) previous_target_ = target_;
        target_ = navigation_wall_ = nullptr;
        collection_ = "visible";
    }
    instruction_ = order;
    kind_ = kind;
    goal_ = goal;
    ++revision_;
    detail_.clear();
    next_ = "player";
    baseline_ = receipt_ = arrival_ = local_observation_ = correction_ = nullptr;
    route_ = nullptr;
    route_committed_ = false;
    disarm();
    return {{"status","updated"},{"executionChanged",true},{"task",context()}};
}

// 复制已可信观察的检查点，并固定任务修订和原话来源。
bool PartnerTask::bind_route(Json& arguments) const {
    if (!navigation() || route_committed_ || route_.is_null() || route_.at("id") != arguments.at("route") ||
        (!navigation_wall_.is_null() && arguments.at("laps") != navigation_wall_.at("laps"))) return false;
    arguments["taskId"] = id_;
    arguments["taskRevision"] = revision_;
    arguments["waypoints"] = route_.at("waypoints");
    return true;
}

// 保存原墙的身份，重开可以换路线但仍需围同一面墙走原圈数。
void PartnerTask::accept_route(const Json& arguments) {
    navigation_wall_ = {{"id",route_.at("wall")},{"observation",route_.at("observation")},
        {"message",route_.at("sourceMessage")},{"laps",arguments.at("laps")}};
    route_committed_ = true;
}

// 禁止已结算的路线仅凭现场变化再次启动。
void PartnerTask::finish_route(bool completed) {
    route_ = nullptr;
    next_ = completed ? "done" : "player";
    disarm();
}

// 原话在其接纳回合登记，之后由任务持有到成功换绑或被更新的纠正取代。
Json PartnerTask::begin_correction(const Json& event,const Json& scene) {
    const auto& message = event.at("sequence");
    if (correcting() && message == correction_.at("message"))
        return {{"status","correction_pending"},{"correction",correction_}};
    if (message.get<std::uint64_t>() <= last_correction_) return {{"status","stale_correction"}};
    correction_ = {{"message",message},{"text",event.at("text")},{"baseRevision",revision_},{"dimension",scene.at("dimension")}};
    last_correction_ = message.get<std::uint64_t>();
    disarm();
    return {{"status","correction_recorded"},{"correction",correction_}};
}

// 记住当前纠正需要等待的旧执行器身份。
void PartnerTask::hold_correction(std::string work) {
    if (correcting()) unconfirmed_work_ = std::move(work);
}

// 保存当前任务说明，只在执行条件变化时推进插件持有的任务修订。
// 步骤：校验正文与原话目标 → 核对执行范围 → 保存约定和唤醒条件。
Json PartnerTask::update(const Json& input, const Json& scene, std::uint64_t scene_revision, const Json* anchor,
    std::uint64_t player_order) {
    auto goal = input.at("goal").get<std::string>();
    auto detail = input.value("detail",goal == goal_ ? detail_ : std::string{});
    auto next = input.at("next").get<std::string>();
    auto collection = input.value("collection",collection_);
    if (navigation() && goal != goal_)
        return {{"status","invalid_target"},{"reason","task_switch_required"}};
    const bool commit = input.value("operation","") == "commit_correction";
    if (commit && (!correcting() || input.at("message") != correction_.at("message") ||
        revision_ != correction_.at("baseRevision") || scene.at("dimension") != correction_.at("dimension")))
        return {{"status","stale_correction"}};
    if (commit && !unconfirmed_work_.empty())
        return {{"status","old_work_exit_unconfirmed"},{"work",unconfirmed_work_}};
    if (correcting() && !commit && (goal != goal_ || collection != collection_ || input.contains("target")))
        return {{"status","correction_pending"},{"correction",correction_}};
    if (commit && (!input.contains("target") || input.at("target").empty()))
        return {{"status","invalid_target"},{"reason","correction_requires_bound_target"}};
    if (has_target() && goal != goal_ && !input.contains("target"))
        return {{"status","invalid_target"},{"reason","target_disposition_required"}};
    if (goal.empty() || goal.size() > 1024 || detail.size() > 2048 ||
        (collection != "visible" && collection != "ground") ||
        (next != "continue" && next != "scene" && next != "player" && next != "done"))
        throw std::invalid_argument("MC partner: invalid task update");
    auto baseline = next == "scene" ? scene_key(scene,scene_revision) : Json{};
    auto target = target_;
    std::uint64_t binding_message = 0;
    if (input.contains("target")) {
        target = input.at("target");
        if (target.empty()) target = nullptr;
        else {
            const auto message = target.at("message").get<std::uint64_t>();
            const bool approach = target.value("approach",false);
            const bool existing = target_.contains("message") && target_.at("message") == message;
            if (commit && !existing && message != correction_.at("message"))
                return {{"status","invalid_target"},{"reason","correction_source_mismatch"}};
            if (commit && existing) anchor = &target_.at("anchor");
            if (!anchor || anchor->at("dimension") != scene.at("dimension") ||
                (anchor->at("position").is_null() && !approach))
                return {{"status","invalid_target"},{"reason","original player position unavailable"}};
            if (!commit && has_target() && !existing && (!target.value("replace",false) || player_order != message))
                return {{"status","invalid_target"},{"reason","new player instruction required to replace the target"}};
            if (approach && !(existing && target_.value("approach",false)) &&
                (player_order != message || target.contains("cells") || target.contains("candidate")))
                return {{"status","invalid_target"},{"reason","record the original approach before choosing cells"}};
            const auto original = openings_.find(message);
            if (target.contains("candidate")) {
                if (target.contains("cells") || approach) return {{"status","invalid_target"},{"reason","choose an original candidate or cells"}};
                if (commit && existing && original != openings_.end()) {
                    const auto choice = target_.value("candidate",original->second.at("playerFocus").at("choice"));
                    if (choice.is_string() && target.at("candidate") != choice)
                        return {{"status","invalid_target"},{"reason","correction_source_mismatch"}};
                }
                auto cells = original == openings_.end() ? Json{} : candidate_cells(original->second,target.at("candidate"));
                if (cells.is_null()) return {{"status","invalid_target"},{"reason","candidate missing from original message observation"}};
                target["cells"] = std::move(cells);
            }
            if (target.contains("cells")) {
                const auto& cells = target.at("cells");
                if (!valid_cells(cells) || (approach && !verified_after_approach(target,scene)) ||
                    (!anchor->at("position").is_null() && !std::ranges::all_of(cells,[&](const auto& cell) {
                        return cell_near_anchor(cell,anchor->at("position")); })))
                    return {{"status","invalid_target"},{"reason","target cells lack current and original coverage"}};
                if (!approach && !commit && target.contains("observation") &&
                    (!verified_cells(target,scene) || local_observation_.at("revision") != revision_))
                    return {{"status","invalid_target"},{"reason","current_observation_required"},{"cells",cells}};
                if (!approach && !target.contains("candidate") && original != openings_.end()) {
                    const auto& choice = original->second.at("playerFocus").at("choice");
                    if (choice.is_string()) {
                        const auto expected = candidate_cells(original->second,choice);
                        if (!expected.is_null() && !std::ranges::is_permutation(expected,cells))
                            return {{"status","invalid_target"},{"reason","cells_conflict_with_pointed_opening"},
                                {"candidate",choice},{"cells",expected}};
                    }
                }
                const auto previous = bound_cells_.find(message);
                if (!commit && previous != bound_cells_.end() && !std::ranges::is_permutation(previous->second,cells))
                    return {{"status","invalid_target"},{"reason","original target cells cannot be changed"}};
                binding_message = message;
            } else if (existing && target_.contains("cells")) {
                return {{"status","invalid_target"},{"reason","original target is already bound"}};
            }
            target.erase("replace");
            target["anchor"] = *anchor;
            if (commit) {
                if (!target.contains("cells") || !verified_cells(target,scene))
                    return {{"status","invalid_target"},{"reason","current_observation_required"},
                        {"cells",target.value("cells",Json::array())}};
                target["instruction"] = correction_.at("message");
            }
        }
    }
    const bool same_target = same_target_scope(target_,target);
    const bool execution_changed = commit || goal_ != goal || collection_ != collection || !same_target;
    if (execution_changed && revision_ == UINT64_MAX) throw std::overflow_error("MC partner: task revision exhausted");
    if (binding_message) {
        if (commit) bound_cells_.insert_or_assign(binding_message,target.at("cells"));
        else bound_cells_.try_emplace(binding_message,target.at("cells"));
        if (bound_cells_.size() > observation_limit) bound_cells_.erase(bound_cells_.begin());
    }
    if (same_target && target.contains("cells")) target["cells"] = target_.at("cells");
    goal_ = std::move(goal);
    detail_ = std::move(detail);
    next_ = std::move(next);
    collection_ = std::move(collection);
    baseline_ = std::move(baseline);
    target_ = std::move(target);
    armed_ = true;
    if (commit) correction_ = nullptr;
    if (execution_changed) {
        ++revision_;
        receipt_ = nullptr;
    }
    return {{"status","updated"},{"executionChanged",execution_changed},{"task",context()}};
}

// 核对最近一次自身读回的身份、维度和时效。
bool PartnerTask::current_observation(const Json& id, const Json& scene) const {
    return id.is_string() && !local_observation_.is_null() && local_observation_.at("id") == id &&
        scene.contains("dimension") && local_observation_.at("dimension") == scene.at("dimension") &&
        std::chrono::steady_clock::now()-observation_time_ <= std::chrono::seconds(30);
}

// 核对插件保存的本轮现场证据及确切坐标，允许原动作已完成的格。
// 步骤：核对观察身份和时效 → 要求全部目标格在本机已加载范围内。
bool PartnerTask::verified_cells(const Json& target,const Json& scene) const {
    if (!target.contains("observation") || !current_observation(target.at("observation"),scene)) return false;
    return std::ranges::all_of(target.at("cells"),[&](const auto& cell) {
        return std::ranges::any_of(local_observation_.at("verified"),[&](const auto& sample) {
            return sample.at("atSelf") == true && sample.at("block").is_string() && same_cell(cell,sample);
        });
    });
}

// 允许原接近请求在实际到达后使用现场明确读回的空格。
// 步骤：核对工作来源和采样时间 → 验证角色在原到达点附近 → 核对每个空格。
bool PartnerTask::verified_after_approach(const Json& target, const Json& scene) const {
    if (arrival_.is_null() || !target.contains("observation") || arrival_.at("order") != target.at("message") ||
        !current_observation(target.at("observation"),scene)) return false;
    const auto& position = arrival_.at("position");
    const auto& observed = local_observation_.at("position");
    for (const auto axis : {"x","y","z"})
        if (std::abs(position.at(axis).get<double>()-observed.at(axis).get<double>()) > 3) return false;
    const auto& verified = local_observation_.at("verified");
    return std::ranges::all_of(target.at("cells"),[&](const auto& cell) {
        return std::ranges::any_of(verified,[&](const auto& sample) {
            return sample.at("atSelf") == true && sample.at("block") == "air" && same_cell(cell,sample);
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

// 固定地面取材范围，提前将当前墙体目标列为保留方格。
void PartnerTask::bind_collection(Json& arguments) const {
    if (collection_ != "ground" && arguments.value("source","") != "ground") return;
    arguments["source"] = "ground";
    if (target_ready()) arguments["protectedCells"] = target_cells();
}

// 分别归档最后一次视线和原话首次确定的墙面，简要观察不会覆盖墙面证据。
// 步骤：更新原话命中 → 固定确切候选 → 回收超出窗口的旧记录。
void PartnerTask::record_focus(std::uint64_t message, const Json& sampled_at, const Json& focus) {
    const Json evidence{{"message",message},{"sampledAtMs",sampled_at},{"playerFocus",focus}};
    observations_[message] = evidence;
    if (focus.contains("objects")) object_observations_[message] = focus;
    if (focus.contains("candidates")) {
        auto [opening,inserted] = openings_.try_emplace(message,evidence);
        if (!inserted && !opening->second.at("playerFocus").at("choice").is_string()) opening->second = evidence;
    }
    if (observations_.size() > observation_limit) observations_.erase(observations_.begin());
    if (object_observations_.size() > observation_limit) object_observations_.erase(object_observations_.begin());
    if (openings_.size() > observation_limit) openings_.erase(openings_.begin());
}

// 分别保存原话指向、实际方块变化和最近一次动作。
// 步骤：归档有来源的观察 → 增补逐格变化 → 缩减网格并更新最近动作。
void PartnerTask::record(const Json& receipt, bool current) {
    if (receipt.at("skill") == "reply" || !receipt.contains("result")) return;
    auto summary = receipt;
    auto& result = summary.at("result");
    if (current && navigation() && receipt.at("skill") == "observe" && result.contains("route")) {
        const auto& route = result.at("route");
        if (route.at("status") == "validated" && route.at("generation") == receipt.at("generation") &&
            (navigation_wall_.is_null() ? route.at("sourceMessage") == id_ || route.at("sourceMessage") == instruction_ :
                route.at("sourceMessage") == navigation_wall_.at("message") && route.at("wall") == navigation_wall_.at("id")))
            route_ = route;
    }
    if (current && receipt.at("skill") == "move" && result.at("status") == "done" && result.value("target",std::string{}) == "player") {
        arrival_ = {{"order",receipt.at("order")},{"position",result.at("position")}};
        local_observation_ = nullptr;
    }
    if (receipt.at("skill") == "observe" && result.contains("anchor") && result.contains("playerFocus")) {
        const auto message = result.at("anchor").at("message").get<std::uint64_t>();
        record_focus(message,result.at("sampledAtMs"),result.at("playerFocus"));
    }
    if (receipt.at("skill") == "observe" && result.contains("observation")) {
        auto& observation = result["observation"];
        auto& samples = observation["samples"];
        if (current && !samples.empty() && result.at("status") == "done" && samples.back().contains("verified")) {
            const auto& sample = samples.back();
            const auto& views = sample.at("views");
            const auto own = std::ranges::find_if(views,[](const auto& view) {
                return view.at("source") == "self" && view.at("status") == "sampled";
            });
            if (own != views.end()) {
                local_observation_ = {{"id",observation.at("id")},{"dimension",observation.at("dimension")},{"revision",revision_},
                    {"position",own->at("actor").at("position")},{"verified",sample.at("verified")}};
                observation_time_ = std::chrono::steady_clock::now();
            }
        }
        for (auto& sample : samples) for (auto& view : sample["views"]) {
            if (view.erase("grid")) view["gridAvailable"] = true;
            if (!view.contains("playerFocus")) continue;
            const auto message = view.at("message").get<std::uint64_t>();
            record_focus(message,sample.at("sampledAtMs"),view.at("playerFocus"));
        }
        for (auto& sample : samples) sample.erase("palette");
    }
    if ((receipt.at("skill") == "collect" || receipt.at("skill") == "place" || receipt.at("skill") == "fill_target") &&
        result.contains("changes")) {
        for (auto cell : result.at("changes")) {
            cell["work"] = receipt.at("bridgeWork");
            cell["order"] = receipt.at("order");
            changes_.push_back(std::move(cell));
        }
        if (changes_.size() > observation_limit)
            changes_.erase(changes_.begin(),changes_.end()-observation_limit);
    }
    if (result.is_object() && result.erase("survey")) result["surveyAvailable"] = true;
    if (current && summary.dump().size() <= 4096) receipt_ = std::move(summary);
}

// 核对自选采掘格来自当前任务有效的自身读回。
// 步骤：核对采样和任务修订 → 找到已加载的确切格 → 核对材质。
std::string_view PartnerTask::dig_observation_issue(const Json& arguments, const Json& scene) const {
    if (!current_observation(arguments.at("observation"),scene) || local_observation_.at("revision") != revision_)
        return "observation_stale";
    const auto& verified = local_observation_.at("verified");
    const auto cell = std::ranges::find_if(verified,[&](const auto& sample) {
        return sample.at("atSelf") == true && sample.at("block").is_string() && same_cell(sample,arguments);
    });
    if (cell == verified.end()) return "observation_cell_unverified";
    return same_material(arguments.at("block").get_ref<const std::string&>(),cell->at("block").get_ref<const std::string&>()) ?
        std::string_view{} : "observation_block_mismatch";
}

// 固定原话命中、唯一可见候选或物体与参照的上下关系。
// 步骤：核对原话材质 → 以可信命中或可见集合确认目标 → 输出执行前需重验的参照格。
bool PartnerTask::bind_dig(std::uint64_t message, const DigIntent& intent, Json& arguments) const {
    const auto original = observations_.find(message);
    if (original == observations_.end() || !same_material(arguments.at("block").get_ref<const std::string&>(),intent.material))
        return false;
    const auto hit = original->second.at("playerFocus").value("hit",Json(nullptr));
    if (intent.support.empty() && same_block(hit,arguments)) return true;
    const auto observed = object_observations_.find(message);
    if (observed == object_observations_.end() || intent.material.empty()) return false;

    const auto& focus = observed->second;
    const auto& ray = focus.at("hit");
    const auto exact = [&](const Json& candidate) {
        if (!candidate.value("visible",false) || !same_material(candidate.at("block").get_ref<const std::string&>(),intent.material))
            return false;
        if (intent.support.empty()) return true;
        const auto below = candidate.value("below",std::string{});
        return !below.empty() && same_material(below,intent.support) && (same_block(ray,candidate) ||
            (ray.is_object() && ray.at("block") == below && ray.at("x") == candidate.at("x") && ray.at("z") == candidate.at("z") &&
                ray.at("y").get<std::int64_t>()+1 == candidate.at("y")));
    };
    const auto& objects = focus.at("objects");
    const auto selected = std::ranges::find_if(objects,exact);
    if (selected == objects.end() || !same_block(*selected,arguments) || std::ranges::count_if(objects,exact) != 1) return false;
    if (!intent.support.empty()) arguments["digReference"] = {{"x",selected->at("x")},{"z",selected->at("z")},
        {"y",selected->at("y").get<std::int64_t>()-1},{"block",selected->at("below")}};
    return true;
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
    if (autonomous && remaining_) --remaining_;
    disarm();
}
}
