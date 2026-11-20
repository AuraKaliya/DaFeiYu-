#include "partner_loop.hpp"

namespace dfy::mc {
using Json = nlohmann::json;
namespace {
// 借用经宿主校验并由原收件引用持有的文本。
std::string_view borrowed(dfy_v2_string_view value) { return {value.data,value.length}; }
}

// 从原生已接纳候选建立一次业务确认，正文保留原参数及原配对玩家。
// 步骤：核对回合身份 → 校验精确采集范围 → 登记候选与期限并按指定渠道发起请求。
bool PartnerLoop::confirm_collect(std::uint64_t session, std::uint64_t request, std::string_view envelope) {
    if (!request || session != session_ || !request_ || confirmation_ || !permits(session) || !feed_.ready()) return false;
    const auto original = Json::parse(envelope);
    const auto& context = original.at("context");
    if (context.at("generation") != session_world_ || context.at("order") != order_) return false;
    auto args = original.at("arguments");
    const auto route = args.value("route","main");
    if (route != "main") return false;
    args.erase("route");
    const auto& block = args.at("block").get_ref<const std::string&>();
    const auto& count = args.at("count");
    if (block.empty() || block.size() > 64 || block.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_:") != block.npos ||
        !count.is_number_unsigned() || count == 0 || count > 32) return false;
    const auto question = "是否采集 "+count.dump()+" 块 "+block+"？";
    Confirmation item{request,order_,0,session_world_,feed_.owner(),question,std::move(args),{},
        std::chrono::steady_clock::now()+std::chrono::seconds(45),route == "game"};
    if (!item.ask_game) publish("mc.confirmation.request",{{"id",request},{"revision",1},{"generation",item.world},
        {"player",item.player},{"order",item.order},{"arguments",item.arguments},{"question",item.question}});
    confirmation_ = std::move(item);
    return true;
}

// 返回唯一已批准候选的不可变参数，动作接纳仍经游戏侧核对当前世界和游标。
std::optional<std::string> PartnerLoop::confirm_result(std::uint64_t request) const {
    if (!confirmation_ || confirmation_->request != request) return std::string{};
    const auto& item = *confirmation_;
    if (item.decision.is_null()) return {};
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
        if (sequence <= item.order || sequence != feed_.cursor() || event.at("type") != "player.message") continue;
        const auto& text = event.at("text");
        if (text == "确认 "+token || text == "approve "+token) { confirm_once("approved","game",sequence); return; }
        if (text == "拒绝 "+token || text == "deny "+token) { confirm_once("denied","game"); return; }
    }
}

// 以本体身份已绑定的定向信箱接收同一确认回复，已裁定时回传原结果。
// 步骤：取件并复制正文 → 核对原编号与来源 → 裁定或复用原业务回执。
void PartnerLoop::intake_confirmation() {
    if (outbound_.size() > 120) return;
    dfy_v2_mailbox_message message{};
    if (mail_->poll(&message,nullptr)) return;
    Json body;
    {
        struct Release {
            const dfy_v2_reference_api* refs;
            dfy_v2_handle payload;
            // 归还已转交的原收件载荷引用。
            ~Release() { refs->release(payload,nullptr); }
        } release{refs_,message.payload};
        dfy_v2_mailbox_text text{};
        if (!mail_text_->inspect(message.payload,&text,nullptr) && text.version == 1 &&
            borrowed(text.category) == "mc.confirmation.response") body = Json::parse(borrowed(text.text),nullptr,false);
    }
    if (!body.is_object() || !body.contains("id") || !body.at("id").is_number_unsigned() ||
        !body.contains("revision") || body.at("revision") != 1 || !body.contains("approve") || !body.at("approve").is_boolean() ||
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
    if (feed_.cursor() != item.order || !feed_.ready()) return;
    if (body.at("approve") == true) confirm_once("approved","main",item.order);
    else confirm_once("denied","main");
}
}
