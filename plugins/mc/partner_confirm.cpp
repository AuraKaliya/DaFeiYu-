#include "partner_loop.hpp"

namespace dfy::mc {
using Json = nlohmann::json;

namespace {
// 为受支持的原生动作生成问题，业务参数保留完整坐标和材料。
std::string confirmation_question(std::string_view skill, const Json& args) {
    const auto& block = args.at("block").get_ref<const std::string&>();
    if (block.empty() || block.size() > 64 || block.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_:") != block.npos)
        throw std::invalid_argument("MC confirmation: invalid block identity");
    if (skill == "collect") {
        const auto& count = args.at("count");
        if (!count.is_number_unsigned() || count == 0 || count > 32)
            throw std::invalid_argument("MC confirmation: invalid collection count");
        return "是否采集 "+count.dump()+" 块 "+block+"？";
    }
    if (skill == "place") {
        for (const auto axis : {"x","y","z"}) if (!args.at(axis).is_number_integer() || args.at(axis).get<double>() < -30000000 ||
            args.at(axis).get<double>() > 30000000) throw std::invalid_argument("MC confirmation: invalid placement coordinate");
        return "是否在 ("+args.at("x").dump()+", "+args.at("y").dump()+", "+args.at("z").dump()+") 放置 "+block+"？";
    }
    throw std::invalid_argument("MC confirmation: unsupported action");
}
}

// 为任一支持确认的候选固定原参数、玩家和渠道。
// 步骤：核对回合身份 → 分离执行参数和确认渠道 → 登记原候选及期限并投递请求。
bool PartnerLoop::confirm_action(std::uint64_t session, std::uint64_t request, std::string_view skill, std::string_view envelope,
    std::string work) {
    if (!request || session != session_ || !request_ || confirmation_ || !permits(session) || !feed_.ready()) return false;
    const auto original = Json::parse(envelope);
    const auto& context = original.at("context");
    if (context.at("generation") != session_world_ || context.at("order") != order_) return false;
    auto args = original.at("arguments");
    const bool legacy = skill == "collect_confirmed";
    if (legacy) skill = "collect";
    const auto route = args.value(legacy ? "route" : "approval","main");
    if (route != "main" && route != "game") return false;
    if (route == "main" && (!question_events_ || question_ || question_work_)) return false;
    args.erase("route");
    args.erase("approval");
    auto question = confirmation_question(skill,args);
    const auto now = std::chrono::steady_clock::now();
    Confirmation item{request,order_,0,session_world_,feed_.owner(),std::string(skill),std::move(question),std::move(args),{},
        now+std::chrono::seconds(25),route == "game"};
    if (!work.empty()) item.work = ApprovalWork{session,request_,task_.revision(),std::move(work)};
    if (!item.ask_game) {
        question_ = Question{request,item.order,0,item.world,item.player,item.question,"main",item.deadline};
        question_->approval = true;
    }
    confirmation_ = std::move(item);
    return true;
}

// 判断当前技能是否仍持有等待或已批准但尚未执行的候选。
bool PartnerLoop::awaiting_approval(std::string_view skill) const noexcept {
    return confirmation_ && confirmation_->work && confirmation_->skill == skill &&
        (confirmation_->decision.is_null() || confirmation_->approved_order);
}

// 检查原候选仍绑定当前游戏世界、玩家和游标。
bool PartnerLoop::confirmation_context_current(std::uint64_t order) const {
    return confirmation_ && order && feed_.ready() && !feed_.departed() && !feed_.gap() &&
        feed_.world() == confirmation_->world && feed_.owner() == confirmation_->player && feed_.cursor() == order &&
        (!confirmation_->work || confirmation_->work->revision == task_.revision());
}

// 返回唯一已批准候选的不可变参数，动作接纳仍经游戏侧核对当前世界和游标。
std::optional<std::string> PartnerLoop::confirm_result(std::uint64_t request) const {
    if (!confirmation_ || confirmation_->request != request) return std::string{};
    const auto& item = *confirmation_;
    if (item.decision.is_null() || (presentation_ && presentation_->request == request)) return {};
    if ((item.ask_game && !item.presented) || !confirmation_context_current(item.approved_order)) return std::string{};
    return Json{{"context",{{"generation",item.world},{"order",item.approved_order}}},{"arguments",item.arguments}}.dump();
}

// 记录第一份通过身份、状态与时效核验的裁定。
void PartnerLoop::confirm_once(std::string_view status, std::string_view source, std::uint64_t approved_order) {
    auto& item = *confirmation_;
    if (!item.decision.is_null()) return;
    Json fact{{"id",item.request},{"revision",1},{"generation",item.world},{"player",item.player},
        {"order",item.order},{"skill",item.skill},{"status",status},{"source",source}};
    publish("mc.confirmation.decision",fact);
    item.decision = std::move(fact);
    item.approved_order = approved_order;
}

// 释放原候选与其提问，保留已经发布的唯一裁定。
void PartnerLoop::confirm_exited(std::uint64_t request) noexcept {
    if (!confirmation_ || confirmation_->request != request) return;
    try { confirm_once("withdrawn","host"); } catch (...) {}
    if (question_ && question_->approval && question_->item == request) question_.reset();
    confirmation_.reset();
}

// 只处理原配对玩家本轮的显式答案，其他聊天仍保留在事件流。
// 步骤：核对原绑定和期限 → 匹配原确认编号 → 先有效者唯一裁定。
void PartnerLoop::advance_confirmation() {
    if (!confirmation_ || !confirmation_->decision.is_null()) return;
    const auto& item = *confirmation_;
    if (feed_.world() != item.world || feed_.owner() != item.player || feed_.gap() || feed_.departed()) {
        confirm_once("withdrawn","host"); return;
    }
    if (item.work && item.work->revision != task_.revision()) { confirm_once("withdrawn","task"); return; }
    if (std::chrono::steady_clock::now() >= item.deadline) { confirm_once("expired","host"); return; }
    if (!item.ask_game || !item.presentation_attempted || !feed_.ready()) return;
    const auto token = std::to_string(item.request);
    for (const auto& event : feed_.pending()) {
        const auto sequence = event.at("sequence").get<std::uint64_t>();
        if (sequence <= item.order || event.at("type") != "player.message") continue;
        const auto& text = event.at("text");
        if (text == "确认 "+token || text == "approve "+token) { confirm_once("approved","game",sequence); return; }
        if (text == "拒绝 "+token || text == "deny "+token) { confirm_once("denied","game"); return; }
    }
}

// 将局内已批准的原候选交给游戏，失效或拒绝的候选只结算待办。
// 步骤：处理关闭 → 核对原裁定与现场 → 接入原工作或报告未执行。
void PartnerLoop::advance_confirmed_work(const BridgeConfig& config, bool closing, bool body_busy) {
    if (!confirmation_ || !confirmation_->work) return;
    auto& item = *confirmation_;
    if (closing) { confirm_exited(item.request); return; }
    const auto envelope = confirm_result(item.request);
    if (!envelope) return;
    if (!envelope->empty() && std::chrono::steady_clock::now() < item.deadline) {
        if (body_busy || motion_) return;
        const std::string_view skill = item.skill == "collect" ? "collect" : "place";
        const auto& work = *item.work;
        auto action = std::make_unique<GameWork>(GameAction::partner,config,work.id,*envelope,item.skill);
        motion_.emplace(Motion{item.order,work.session,work.turn,item.request,item.world,work.id,skill,std::move(action)});
        motion_->accepted = false;
    } else {
        auto fact = item.decision;
        fact["execution"] = item.approved_order ? "stale" : "skipped";
        publish("mc.confirmation.execution",fact);
        approval_result_ = std::move(fact);
    }
    confirm_exited(item.request);
}

// 经实际游戏发言票据呈现原问题，显示渠道和最终批准各自留有回执。
// 步骤：推进原发言到真实退出 → 报告实效 → 在当前绑定下最多开始一次呈现。
void PartnerLoop::advance_confirmation_presentation(const BridgeConfig& config, std::uint64_t instance, bool closing) {
    if (presentation_) {
        auto& pending = *presentation_;
        std::optional<GameResult> result;
        try { result = pending.work->poll(closing || feed_.world() != pending.world); }
        catch (const std::exception&) { result = GameResult{false,GameEffect::unknown,{}}; }
        if (!result) return;
        const auto receipt = Json::parse(result->payload,nullptr,false);
        const bool sent = receipt.is_object() && receipt.value("sent",false);
        if (confirmation_ && confirmation_->request == pending.request) confirmation_->presented = sent;
        publish("mc.confirmation.presentation",{{"id",pending.request},{"revision",1},{"order",pending.order},
            {"generation",pending.world},{"player",pending.player},{"bridgeWork",pending.work_id},
            {"status",sent ? "sent" : "unconfirmed"}});
        presentation_.reset();
    }
    if (closing || !confirmation_ || !confirmation_->decision.is_null()) return;
    auto& item = *confirmation_;
    if (!item.ask_game || item.presentation_attempted) return;
    if (!feed_.ready() || (!item.work && feed_.cursor() != item.order) || feed_.world() != item.world || feed_.owner() != item.player) {
        publish("mc.confirmation.presentation",{{"id",item.request},{"revision",1},{"order",item.order},
            {"generation",item.world},{"player",item.player},{"status","unconfirmed"}});
        item.presentation_attempted = true;
        return;
    }
    const auto number = std::to_string(item.request);
    const auto question = item.question+" 回复 确认 "+number+" 或 拒绝 "+number;
    const auto envelope = Json{{"context",{{"generation",item.world},{"order",feed_.cursor()}}},{"arguments",{{"text",question}}}}.dump();
    auto role = "confirm."+std::to_string(instance)+"."+number;
    auto work = std::make_unique<GameWork>(GameAction::partner,config,role,envelope,"reply");
    presentation_ = Presentation{item.request,item.order,item.world,item.player,std::move(role),std::move(work)};
    item.presentation_attempted = true;
}
}
