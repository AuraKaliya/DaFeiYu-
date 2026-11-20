#include "partner_loop.hpp"

namespace dfy::mc {
using Json = nlohmann::json;

// 从原生已接纳候选建立一次业务确认，正文保留原参数及原配对玩家。
// 步骤：核对回合身份 → 校验精确采集范围 → 登记候选与期限并按指定渠道发起请求。
bool PartnerLoop::confirm_collect(std::uint64_t session, std::uint64_t request, std::string_view envelope) {
    if (!request || session != session_ || !request_ || confirmation_ || !permits(session) || !feed_.ready()) return false;
    const auto original = Json::parse(envelope);
    const auto& context = original.at("context");
    if (context.at("generation") != session_world_ || context.at("order") != order_) return false;
    auto args = original.at("arguments");
    const auto route = args.value("route","main");
    if (route != "main" && route != "game") return false;
    args.erase("route");
    const auto& block = args.at("block").get_ref<const std::string&>();
    const auto& count = args.at("count");
    if (block.empty() || block.size() > 64 || block.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_:") != block.npos ||
        !count.is_number_unsigned() || count == 0 || count > 32) return false;
    const auto question = "是否采集 "+count.dump()+" 块 "+block+"？";
    Confirmation item{request,order_,0,session_world_,feed_.owner(),question,std::move(args),{},
        std::chrono::steady_clock::now()+std::chrono::seconds(25),route == "game"};
    if (!item.ask_game) publish("mc.confirmation.request",{{"id",request},{"revision",1},{"generation",item.world},
        {"player",item.player},{"order",item.order},{"arguments",item.arguments},{"question",item.question}});
    confirmation_ = std::move(item);
    return true;
}

// 返回唯一已批准候选的不可变参数，动作接纳仍经游戏侧核对当前世界和游标。
std::optional<std::string> PartnerLoop::confirm_result(std::uint64_t request) const {
    if (!confirmation_ || confirmation_->request != request) return std::string{};
    const auto& item = *confirmation_;
    if (item.decision.is_null() || (presentation_ && presentation_->request == request)) return {};
    if (!item.approved_order || !feed_.ready() || feed_.world() != item.world || feed_.owner() != item.player ||
        feed_.cursor() != item.approved_order || feed_.gap()) return std::string{};
    return Json{{"context",{{"generation",item.world},{"order",item.approved_order}}},{"arguments",item.arguments}}.dump();
}

// 记录第一份通过身份、状态与时效核验的裁定。
void PartnerLoop::confirm_once(std::string_view status, std::string_view source, std::uint64_t approved_order) {
    auto& item = *confirmation_;
    if (!item.decision.is_null()) return;
    Json fact{{"id",item.request},{"revision",1},{"generation",item.world},{"player",item.player},
        {"order",item.order},{"status",status},{"source",source}};
    publish("mc.confirmation.decision",fact);
    item.decision = std::move(fact);
    item.approved_order = approved_order;
}

// 保留真实业务裁定供原身份重读，释放后不给新动作继承旧批准。
// 步骤：检查唯一待定对象 → 尝试记录失效 → 保留终态并归还原候选。
void PartnerLoop::confirm_exited(std::uint64_t request) noexcept {
    if (!confirmation_ || confirmation_->request != request) return;
    try { confirm_once("withdrawn","host"); } catch (...) {}
    decisions_[next_decision_] = std::move(confirmation_->decision);
    next_decision_ = (next_decision_+1)%decisions_.size();
    confirmation_.reset();
}

// 只处理原配对玩家本轮的显式答案，其他聊天仍保留在事件流。
// 步骤：核对原绑定和期限 → 匹配原确认编号 → 先有效者唯一裁定。
void PartnerLoop::advance_confirmation() {
    if (!confirmation_ || !confirmation_->decision.is_null()) return;
    const auto& item = *confirmation_;
    if (feed_.world() != item.world || feed_.owner() != item.player || feed_.gap()) { confirm_once("withdrawn","host"); return; }
    if (std::chrono::steady_clock::now() >= item.deadline) { confirm_once("expired","host"); return; }
    if (!feed_.ready()) return;
    const auto token = std::to_string(item.request);
    for (const auto& event : feed_.pending()) {
        const auto sequence = event.at("sequence").get<std::uint64_t>();
        if (sequence <= item.order || event.at("type") != "player.message") continue;
        const auto& text = event.at("text");
        if (text == "确认 "+token || text == "approve "+token) { confirm_once("approved","game",sequence); return; }
        if (text == "拒绝 "+token || text == "deny "+token) { confirm_once("denied","game"); return; }
    }
}

// 处理同一确认的本体回信，已裁定时回传原结果。
// 步骤：核对正文和原编号 → 复用已经裁定的结果 → 呈现问题或采用有效答复。
void PartnerLoop::intake_confirmation(bool present, const Json& body) {
    if (!body.is_object() || !body.contains("id") || !body.at("id").is_number_unsigned() ||
        !body.contains("revision") || body.at("revision") != 1 || (!present && (!body.contains("approve") || !body.at("approve").is_boolean())) ||
        !body.contains("generation") || !body.at("generation").is_string() || !body.contains("player") || !body.at("player").is_string()) return;
    const auto id = body.at("id").get<std::uint64_t>();
    const auto same = [&body,id](const Json& item) {
        return item.is_object() && item.at("id") == id && item.at("generation") == body.value("generation","") &&
            item.at("player") == body.value("player","");
    };
    for (const auto& decision : decisions_) if (same(decision)) { publish("mc.confirmation.decision",decision); return; }
    if (!confirmation_ || confirmation_->request != id || confirmation_->world != body.value("generation","") ||
        confirmation_->player != body.value("player","")) return;
    const auto& item = *confirmation_;
    if (same(item.decision)) { publish("mc.confirmation.decision",item.decision); return; }
    if (present) { confirmation_->ask_game = true; return; }
    if (!feed_.ready()) return;
    if (body.at("approve") == true) confirm_once("approved","main",item.order);
    else confirm_once("denied","main");
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
        publish("mc.confirmation.presentation",{{"id",pending.request},{"revision",1},{"order",pending.order},
            {"generation",pending.world},{"player",pending.player},{"status",sent ? "sent" : "unconfirmed"}});
        presentation_.reset();
    }
    if (closing || !confirmation_ || !confirmation_->decision.is_null()) return;
    auto& item = *confirmation_;
    if (!item.ask_game || item.presentation_attempted) return;
    if (!feed_.ready() || feed_.cursor() != item.order || feed_.world() != item.world || feed_.owner() != item.player) {
        publish("mc.confirmation.presentation",{{"id",item.request},{"revision",1},{"order",item.order},
            {"generation",item.world},{"player",item.player},{"status","unconfirmed"}});
        item.presentation_attempted = true;
        return;
    }
    const auto number = std::to_string(item.request);
    const auto question = item.question+" 回复 确认 "+number+" 或 拒绝 "+number;
    const auto envelope = Json{{"context",{{"generation",item.world},{"order",item.order}}},{"arguments",{{"text",question}}}}.dump();
    const auto role = "confirm."+std::to_string(instance)+"."+number;
    auto work = std::make_unique<GameWork>(GameAction::partner,config,role,envelope,"reply");
    presentation_ = Presentation{item.request,item.order,item.world,item.player,std::move(work)};
    item.presentation_attempted = true;
}
}
