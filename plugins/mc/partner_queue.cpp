#include "partner_loop.hpp"
#include <algorithm>
#include <iterator>

namespace dfy::mc {
namespace {
using Json = nlohmann::json;
constexpr std::size_t queue_limit = 4;

// 判断步骤是否只携带一种坐标目标及对应的已支持参数。
// 步骤：核对移动与跟随的独立参数 → 校验精确方块及采集数量。
bool valid_step(std::string_view skill, const Json& args) {
    if (!args.is_object() || args.dump().size() > 512) return false;
    if (skill == "follow") return args.empty() || (args.size() == 1 && args.contains("distance") &&
        args.at("distance").is_number_integer() && args.at("distance") >= 2 && args.at("distance") <= 5);
    if (skill == "move" && args.size() == 1) return (args.contains("place") && args.at("place").is_string()) ||
        (args.contains("message") && args.at("message").is_number_unsigned() && args.at("message") > 0);
    for (const auto axis : {"x","y","z"}) {
        if (!args.contains(axis) || !args.at(axis).is_number_integer() ||
            args.at(axis) < -30000000 || args.at(axis) > 30000000) return false;
    }
    if (skill == "move") return args.size() == 3;
    if (!args.contains("block") || !args.at("block").is_string()) return false;
    if (skill == "place") return args.size() == 4;
    return skill == "collect" && args.size() == 5 && args.contains("count") && args.at("count") == 1;
}
}

// 接纳带修订的模型意图，替换时只有全部新步骤通过才能停止旧工作。
// 步骤：核对操作与旧工作 → 校验本轮工具范围及步骤参数 → 更新待执行队列。
nlohmann::json PartnerLoop::queue_actions(std::uint64_t invocation, const Json& args) {
    const auto operation = args.at("operation").get<std::string>();
    if (operation == "clear") {
        clear_queue("cleared");
        return {{"status","cleared"}};
    }
    const bool replace = operation == "replace";
    if ((!replace && operation != "append") ||
        (replace && (!motion_ || !args.contains("work") || args.at("work") != motion_->work)))
        return {{"status","stale"}};

    const auto& steps = args.at("steps");
    const auto waiting = replace ? 0 : queued_.size();
    if (!steps.is_array() || steps.empty() || waiting+steps.size() > queue_limit)
        throw std::invalid_argument("MC queue: expected one to four steps");
    std::deque<QueuedAction> pending;
    Json ids = Json::array();
    const auto enabled = scope();
    for (const auto& step : steps) {
        const auto name = step.at("skill").get<std::string>();
        const auto& parameters = step.at("arguments");
        const auto skill = std::ranges::find(partner_skills,name,&PartnerSkill::name);
        if (skill == partner_skills.end() || !valid_step(name,parameters) ||
            (skill->category != SkillCategory::movement && !enabled.test(static_cast<std::size_t>(skill->category))) ||
            (name == "place" && !task_.target_ready()) || awaiting_approval(name))
            throw std::invalid_argument("MC queue: step is outside the current action scope");
        auto id = "queued."+std::to_string(invocation)+"."+std::to_string(pending.size()+1);
        ids.push_back(id);
        pending.push_back({order_,request_,invocation,task_.revision(),std::move(id),skill->name,parameters});
    }

    if (replace) {
        clear_queue("replaced");
        motion_->stopping = true;
        replacing_motion_ = true;
    }
    queued_.insert(queued_.end(),std::make_move_iterator(pending.begin()),std::make_move_iterator(pending.end()));
    publish("mc.partner.queue",{{"status","queued"},{"works",ids},{"revision",task_.revision()},{"order",order_}});
    return {{"status","queued"},{"works",std::move(ids)}};
}

// 释放只在插件内登记、还未发送给游戏的步骤。
void PartnerLoop::clear_queue(std::string_view reason) {
    if (queued_.empty()) return;
    Json ids = Json::array();
    for (const auto& step : queued_) ids.push_back(step.work);
    publish("mc.partner.queue",{{"status",reason},{"works",std::move(ids)}});
    queued_.clear();
}

// 将队首变为唯一占用角色的游戏工作，原输入和实际接纳游标分别保留。
// 步骤：处理配对和修订失效 → 等待角色控制权 → 重验动作并交给桥接。
void PartnerLoop::advance_queue(const BridgeConfig& config, bool closing, bool body_busy) {
    if (queued_.empty()) return;
    const auto& step = queued_.front();
    if (closing || feed_.gap() || step.revision != task_.revision()) { clear_queue("stale"); return; }
    if (body_busy || motion_ || !feed_.ready() || !feed_.pending().empty()) return;
    auto args = step.arguments;
    if (awaiting_approval(step.skill) || (step.skill == "place" && !task_.bind_placement(args))) {
        clear_queue("blocked");
        return;
    }
    const auto envelope = Json{{"context",{{"generation",session_world_},{"order",feed_.cursor()}}},
        {"arguments",std::move(args)}}.dump();
    auto action = std::make_unique<GameWork>(GameAction::partner,config,step.work,envelope,std::string(step.skill));
    motion_.emplace(Motion{step.order,session_,step.request,step.invocation,session_world_,step.work,step.skill,std::move(action)});
    motion_->accepted = false;
    if (step.skill == "collect") {
        motion_->pickup = step.arguments.at("block").get<std::string>();
        if (motion_->pickup.starts_with("minecraft:")) motion_->pickup.erase(0,10);
    }
    queued_.pop_front();
}
}
