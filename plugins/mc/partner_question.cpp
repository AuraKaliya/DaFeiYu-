#include "partner_loop.hpp"
#include <algorithm>

namespace dfy::mc {
namespace {
using Json = nlohmann::json;
constexpr std::string_view help_type = "mc.partner.help", feedback_type = "mc.ask.feedback";
constexpr std::string_view receipt_type = "mc.ask.receipt";

// 向 C ABI 借出宿主当前调用期间有效的文本。
dfy_v2_string_view view(std::string_view value) { return {value.data(),value.size()}; }

// 把插件构造的通用投影复制给宿主。
uint32_t project(const Json& body, uint32_t kind, dfy_v2_event_projection* out) {
    const auto wire = body.dump();
    if (!out || wire.size() > sizeof(out->json)) return DFY_V2_INVALID_ARGUMENT;
    std::copy(wire.begin(),wire.end(),out->json);
    out->length = static_cast<uint32_t>(wire.size());
    out->kind = kind;
    return DFY_V2_OK;
}

// 将 MC 疑问转换为已核对的通用提问或主 Agent 信件。
uint32_t DFY_V2_CALL decode_help(void*, dfy_v2_string_view raw, dfy_v2_event_projection* out) {
    try {
        auto body = Json::parse(std::string_view(raw.data,raw.length));
        const auto reason = body.at("reason");
        if (reason != "local_timeout" && reason != "action_approval") return DFY_V2_INVALID_ARGUMENT;
        const auto mode = body.at("path");
        if (mode != "main" && mode != "direct") return DFY_V2_INVALID_ARGUMENT;
        if (reason == "action_approval") body["choices"] = {"approve","deny"};
        body["context"] = Json::parse(body.at("context").get<std::string>());
        body.erase("path");
        return project(body,mode == "main" ? DFY_V2_EVENT_ASK_VIA_MAIL : DFY_V2_EVENT_ASK,out);
    } catch (...) { return DFY_V2_FAILED; }
}

// 将游戏渠道实际呈现和玩家答复交给同一个 ASK 票据。
uint32_t DFY_V2_CALL decode_feedback(void*, dfy_v2_string_view raw, dfy_v2_event_projection* out) {
    try { return project(Json::parse(std::string_view(raw.data,raw.length)),DFY_V2_EVENT_ASK_FEEDBACK,out); }
    catch (...) { return DFY_V2_FAILED; }
}

// 将插件消费原终态信件的回执投影给交互路由。
uint32_t DFY_V2_CALL decode_receipt(void*, dfy_v2_string_view raw, dfy_v2_event_projection* out) {
    try { return project(Json::parse(std::string_view(raw.data,raw.length)),DFY_V2_EVENT_ASK_RECEIPT,out); }
    catch (...) { return DFY_V2_FAILED; }
}
}

// 在插件加载时登记求助和反馈的精确版本及结构。
// 步骤：查询可选能力 → 登记求助与两类回执 → 保存提交接口。
uint32_t PartnerLoop::register_questions(dfy_v2_query_interface_fn host) {
    const void* table = nullptr;
    if (const auto status = host(view(DFY_V2_ROUTED_EVENT_INTERFACE),1,&table,nullptr); status) return status;
    const auto* events = static_cast<const dfy_v2_routed_event_api*>(table);
    if (!events || events->header.struct_size != sizeof(*events) || events->header.interface_version != 1 ||
        !events->register_event || !events->emit) return DFY_V2_ABI_MISMATCH;
    if (const auto status = events->register_event(view(help_type),1,
        view(R"({"item":"uint","revision":"uint","question":"string","reason":"string","path":"string","recipient":"string","channel":"string","waitMs":"uint","context":"string"})"),
        decode_help,nullptr,nullptr); status) return status;
    if (const auto status = events->register_event(view(feedback_type),1,
        view(R"({"ask":"uint","channel":"string","action":"string","delivered":"bool","recipient":"string","answer":"string","bridgeWork":"string"})"),
        decode_feedback,nullptr,nullptr); status) return status;
    if (const auto status = events->register_event(view(receipt_type),1,
        view(R"({"ask":"uint","item":"uint","revision":"uint","eventDomain":"uint","event":"uint","disposition":"string"})"),
        decode_receipt,nullptr,nullptr); status) return status;
    if (const auto status = host(view(DFY_V2_ROUTED_MAILBOX_INTERFACE),1,&table,nullptr); status) return status;
    const auto* mail = static_cast<const dfy_v2_routed_mailbox_api*>(table);
    if (!mail || mail->header.struct_size != sizeof(*mail) || !mail->register_mailbox) return DFY_V2_ABI_MISMATCH;
    if (const auto status = mail->register_mailbox(view("mc.partner.question"),1,DFY_V2_MAIL_HIGH | DFY_V2_MAIL_WAKE,nullptr); status) return status;
    question_events_ = events;
    return DFY_V2_OK;
}

// 保留原世界、玩家和问题，等待插件循环开始呈现。
nlohmann::json PartnerLoop::open_question(std::uint64_t id, const Json& args) {
    if (!question_events_ || !id) throw std::invalid_argument("MC question: unavailable");
    if (question_) return {{"status","waiting"},{"item",question_->item}};
    const auto text = args.at("question").get<std::string>(), path = args.value("path",std::string("direct"));
    if (text.empty() || text.size() > 400 || (path != "direct" && path != "main")) throw std::invalid_argument("MC question: invalid request");
    const auto now = std::chrono::steady_clock::now();
    question_ = Question{id,order_,0,session_world_,feed_.owner(),text,path,now+std::chrono::seconds(6),now+std::chrono::seconds(40)};
    question_result_.clear();
    return {{"status","recorded"},{"item",id}};
}

// 确认收到终态，只有仍匹配的事项可以消费答复。
// 步骤：读取事项与原事件身份 → 分流呈现或终态 → 结算业务并确认收件。
void PartnerLoop::intake_question(std::string_view category, const Json& body) {
    if (!question_events_ || !body.is_object()) return;
    const auto item = body.value("item",std::uint64_t{});
    const auto domain = body.value("eventDomain",std::uint64_t{}), event = body.value("event",std::uint64_t{});
    if (!item || !domain || !event || body.value("revision",0) != 1) return;
    const auto id = body.value("ask",std::uint64_t{});
    const bool matched = question_ && item == question_->item && domain == question_->event_domain && event == question_->event_sequence &&
        (!question_->ask || question_->ask == id) && body.value("recipient",std::string{}) == question_->player &&
        question_->world == feed_.world() && question_->player == feed_.owner() && !feed_.departed();
    if (category == "interaction.ask.present") {
        if (matched && id && !question_->ask && !question_work_ && body.value("question",std::string{}) == question_->text) {
            question_->ask = id;
            question_->presented = false;
        }
        return;
    }
    const bool current = matched && (!question_->approval || (confirmation_ && confirmation_->request == item &&
        confirmation_->decision.is_null() && std::chrono::steady_clock::now() < confirmation_->deadline &&
        confirmation_context_current(body.at("channel") == "game" ? question_->answer_order : question_->order)));
    if (matched)
        complete_question(body.at("status").get<std::string>(),body.at("answer").get<std::string>(),body.at("channel").get<std::string>());
    question_outbound_.emplace_back(receipt_type,Json{{"ask",id},{"item",item},{"revision",1},{"eventDomain",domain},
        {"event",event},{"disposition",current ? "matched" : "stale"}});
}

// 把已核对的答复作为插件局内事实提交给后续上下文。
void PartnerLoop::complete_question(std::string_view status, std::string answer, std::string_view channel) {
    if (question_->approval) {
        if (confirmation_ && confirmation_->request == question_->item) {
            const auto order = channel == "game" ? question_->answer_order : question_->order;
            const auto source = channel == "game" ? "game" : "main";
            if (std::chrono::steady_clock::now() >= confirmation_->deadline) confirm_once("expired","host");
            else if (status == "answered" && answer == "approve" && confirmation_context_current(order)) confirm_once("approved",source,order);
            else if (status == "answered" && answer == "deny") confirm_once("denied",source);
            else if (status == "expired") confirm_once("expired","host");
            else confirm_once("withdrawn","host");
        }
        question_.reset();
        return;
    }
    question_result_ = {{"item",question_->item},{"question",question_->text},{"status",status},{"answer",std::move(answer)}};
    question_.reset();
    question_wake_ = true;
}

// 跳过已退役的原求助，宿主容量暂满时保留原事件及发送序号。
// 步骤：核对队首求助 → 提交并保留暂满项 → 记录接纳身份并出队。
void PartnerLoop::flush_question_events() {
    while (!question_outbound_.empty() && question_outbound_.front().first == help_type &&
        (!question_ || question_outbound_.front().second.at("item") != question_->item)) question_outbound_.pop_front();
    if (question_outbound_.empty()) return;
    const auto& [type,body] = question_outbound_.front();
    const auto wire = body.dump();
    dfy_v2_event_receipt receipt{};
    const auto status = question_events_->emit(view(type),1,question_sequence_+1,view(wire),&receipt,nullptr);
    if (status == DFY_V2_RESOURCE_EXHAUSTED) return;
    if (status || receipt.sequence != question_sequence_+1) throw std::runtime_error("MC question: event rejected");
    if (type == help_type) {
        question_->event_domain = receipt.domain;
        question_->event_sequence = receipt.sequence;
    }
    ++question_sequence_;
    question_outbound_.pop_front();
}

// 推进本地问题、超时升级、实际呈现及可信玩家答复。
// 步骤：结算原发言 → 核对世界与时限 → 尝试答复、升级或开启呈现。
void PartnerLoop::advance_question(const BridgeConfig& config, std::uint64_t instance, bool closing) {
    const bool live = !closing && question_ && question_->world == feed_.world() && question_->player == feed_.owner() && !feed_.departed();
    if (question_work_) {
        std::optional<GameResult> result;
        try { result = question_work_->poll(!live); }
        catch (const std::exception&) { result = GameResult{false,GameEffect::unknown,{}}; }
        if (!result) return;
        question_work_.reset();
        if (live) {
            auto& question = *question_;
            const auto receipt = Json::parse(result->payload,nullptr,false);
            question.presented = result->success && receipt.is_object() && receipt.value("sent",false);
            if (question.ask) question_outbound_.emplace_back(feedback_type,Json{{"ask",question.ask},{"channel","game"},
                {"action","presented"},{"delivered",question.presented},{"recipient",question.player},{"answer",""},
                {"bridgeWork","ask."+std::to_string(instance)+".r."+std::to_string(question.ask)}});
        }
    }
    if (!live) { question_.reset(); flush_question_events(); return; }
    auto& q = *question_;
    const auto now = std::chrono::steady_clock::now();
    if (!q.escalated && now >= q.local_answer_deadline) { complete_question("expired",{}); return; }
    if (q.presented && !q.forwarded) {
        const auto prefix = "答复 "+std::to_string(q.ask ? q.ask : q.item)+" ";
        const auto token = std::to_string(q.ask);
        for (const auto& event : feed_.pending()) {
            const auto text = event.value("text",std::string{});
            if (event.at("sequence") <= q.order) continue;
            std::string answer;
            if (q.approval && (text == "确认 "+token || text == "approve "+token)) answer = "approve";
            else if (q.approval && (text == "拒绝 "+token || text == "deny "+token)) answer = "deny";
            else if (text.starts_with(prefix)) answer = text.substr(prefix.size());
            if (answer.empty() || (q.approval && answer != "approve" && answer != "deny")) continue;
            if (!q.escalated) { complete_question("answered",answer); return; }
            if (!q.ask) continue;
            question_outbound_.emplace_back(feedback_type,Json{{"ask",q.ask},{"channel","game"},{"action","answer"},
                {"delivered",false},{"recipient",q.player},{"answer",answer},{"bridgeWork",""}});
            q.answer_order = event.at("sequence");
            q.forwarded = true;
            break;
        }
    }
    if (!q.escalated && now >= q.local) {
        Json context = Json::object();
        if (q.approval) context = {{"generation",q.world},{"order",q.order},{"skill",confirmation_->skill},
            {"arguments",confirmation_->arguments}};
        question_outbound_.emplace_back(help_type,Json{{"item",q.item},{"revision",1},{"question",q.text},
            {"reason",q.approval ? "action_approval" : "local_timeout"},{"context",context.dump()},
            {"path",q.path},{"recipient",q.player},{"channel","game"},{"waitMs",24000}});
        q.escalated = true;
    }
    const bool start = (!q.initial_started || (q.ask && !q.ask_started)) && feed_.ready();
    if (start) {
        if (q.approval && feed_.cursor() != q.order) {
            question_outbound_.emplace_back(feedback_type,Json{{"ask",q.ask},{"channel","game"},
                {"action","presented"},{"delivered",false},{"recipient",q.player},{"answer",""},{"bridgeWork",""}});
            q.ask_started = true;
            flush_question_events();
            return;
        }
        const auto token = std::to_string(q.ask ? q.ask : q.item);
        const auto prompt = q.text+(q.approval ? " 回复 确认 "+token+" 或 拒绝 "+token : " 回复 答复 "+token+" <回答>");
        const auto arguments = Json{{"context",{{"generation",q.world},{"order",feed_.cursor()}}},
            {"arguments",{{"text",prompt}}}}.dump();
        question_work_ = std::make_unique<GameWork>(GameAction::partner,config,
            "ask."+std::to_string(instance)+"."+(q.ask ? "r." : "l.")+token,arguments,"reply");
        if (q.ask) q.ask_started = true;
        q.initial_started = true;
    }
    flush_question_events();
}
}
