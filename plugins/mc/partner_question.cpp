#include "partner_loop.hpp"
#include "partner_effect.hpp"
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
        if (reason != "clarification" && reason != "action_approval") return DFY_V2_INVALID_ARGUMENT;
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
    if (const auto status = events->register_event(view(receipt_type),2,
        view(R"({"ask":"uint","item":"uint","revision":"uint","eventDomain":"uint","event":"uint","disposition":"string","mailbox":"uint","message":"uint"})"),
        decode_receipt,nullptr,nullptr); status) return status;
    if (const auto status = host(view(DFY_V2_ROUTED_MAILBOX_INTERFACE),1,&table,nullptr); status) return status;
    const auto* mail = static_cast<const dfy_v2_routed_mailbox_api*>(table);
    if (!mail || mail->header.struct_size != sizeof(*mail) || !mail->register_mailbox) return DFY_V2_ABI_MISMATCH;
    if (const auto status = mail->register_mailbox(view("mc.partner.question"),1,DFY_V2_MAIL_HIGH | DFY_V2_MAIL_WAKE,nullptr); status) return status;
    question_events_ = events;
    return DFY_V2_OK;
}

// 保存现地追问及目标确定后要继续的操作。
// 步骤：验证渠道及问题 → 核对可选后继能力 → 按玩家原话和任务修订登记。
nlohmann::json PartnerLoop::open_question(std::uint64_t id, const Json& args) {
    if (!id) throw std::invalid_argument("MC question: unavailable");
    if (question_) return {{"status","waiting"},{"item",question_->item}};
    if (question_work_) return {{"status","busy"}};
    const auto text = args.at("question").get<std::string>(), path = args.value("path",std::string("local"));
    const auto options = args.value("options",Json::array());
    if (text.empty() || text.size() > 400 || !options.is_array() || options.size() > 4 ||
        (path != "local" && (!question_events_ || (path != "direct" && path != "main"))))
        throw std::invalid_argument("MC question: invalid request");
    for (const auto& option : options) if (!option.is_string() || option.get_ref<const std::string&>().empty() ||
        option.get_ref<const std::string&>().size() > 160) throw std::invalid_argument("MC question: invalid option");
    const auto after_pickup = args.value("afterPickup",Json(nullptr));
    if (args.contains("afterPickup") && (path != "local" || events_.empty() || events_.back().at("type") != "player.message" ||
        !after_pickup.is_object() ||
        after_pickup.size() != 2+after_pickup.count("pickup") ||
        item_name(after_pickup.at("item").get_ref<const std::string&>()).empty() ||
        !after_pickup.at("count").is_number_integer() || after_pickup.at("count") < 1 || after_pickup.at("count") > 32 ||
        (after_pickup.contains("pickup") && item_name(after_pickup.at("pickup").get_ref<const std::string&>()).empty())))
        throw std::invalid_argument("MC question: invalid pickup continuation");
    question_ = Question{id,order_,0,session_world_,feed_.owner(),text,path,std::chrono::steady_clock::now()+std::chrono::seconds(40)};
    question_->options = options;
    question_->after_pickup = after_pickup;
    question_->task_revision = task_.revision();
    if (!events_.empty()) question_->origin = events_.back().value("text",std::string{});
    question_result_.clear();
    return {{"status","recorded"},{"item",id}};
}

// 确认收到终态，只有仍匹配的事项可以消费答复。
// 步骤：读取事项与原事件身份 → 分流呈现或终态 → 结算业务并确认收件。
void PartnerLoop::intake_question(std::string_view category, const Json& body, const dfy_v2_mailbox_message& message) {
    if (!question_events_ || !body.is_object()) return;
    const auto item = body.value("item",std::uint64_t{});
    const auto domain = body.value("eventDomain",std::uint64_t{}), event = body.value("event",std::uint64_t{});
    if (!item || !domain || !event || body.value("revision",0) != 1) return;
    const auto id = body.value("ask",std::uint64_t{});
    const bool matched = question_ && item == question_->item && domain == question_->event_domain && event == question_->event_sequence &&
        (!question_->ask || question_->ask == id) && body.value("recipient",std::string{}) == question_->player &&
        question_->world == feed_.world() && question_->player == feed_.owner() && !feed_.departed() && !feed_.departure();
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
        {"event",event},{"disposition",current ? "matched" : "stale"},{"mailbox",message.mailbox},{"message",message.sequence}});
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
    if (question_->answer_order) question_result_["answerMessage"] = question_->answer_order;
    question_.reset();
    question_wake_ = status != "resolved" && status != "superseded";
}

// 将局内追问限制在原任务，已核实的显式答复可解除对应动作的等待。
bool PartnerLoop::waits_for_player(std::uint64_t session, std::uint64_t resolves) const noexcept {
    return session == session_ && !active_delegation_ && question_ && question_->path == "local" &&
        !question_->approval && question_->task_revision == task_.revision() && resolves != question_->item;
}

// 只有当前玩家提供了新指向且同轮观察支持原动作时才注销追问。
// 步骤：核对事项和玩家原话 → 用可信观察匹配动作 → 记录无需唤醒的结算。
void PartnerLoop::resolve_block_question(std::uint64_t item, std::string_view envelope) {
    if (!item || !question_ || question_->item != item || question_->path != "local" || question_->approval ||
        order_ <= question_->order || events_.empty() || events_.back().at("type") != "player.message") return;
    auto args = Json::parse(envelope).at("arguments");
    if (!pointed_dig(session_,order_,item,args).value_or(false)) return;
    complete_pointed_question();
}

// 把解开的自然追问与提供新指向的原话绑定。
void PartnerLoop::complete_pointed_question() {
    question_->answer_order = order_;
    complete_question("resolved",events_.back().at("text").get<std::string>());
}

// 校验模型所选的格确实来自本轮玩家指向，必要时同时核对待答事项。
// 步骤：识别当前指向 → 核对原问题身份 → 匹配坐标和材质。
std::optional<bool> PartnerLoop::pointed_dig(std::uint64_t session, std::uint64_t message, std::uint64_t item,
    Json& args) const {
    if (session != session_ || active_delegation_) return !message && !item ? std::optional(false) : std::nullopt;
    const bool player_event = !events_.empty() && events_.back().at("type") == "player.message";
    auto intent = player_event ? read_dig_intent(events_.back().at("text").get_ref<const std::string&>()) : DigIntent{};
    if (!message && !item && !intent.pointed && !hit_queries_.contains(order_)) return false;
    if (!message) message = order_;
    if (message != order_ || !player_event ||
        (item && (!question_ || question_->item != item || question_->approval || question_->path != "local" ||
            question_->order >= order_ || question_->task_revision != task_.revision()))) return std::nullopt;
    if (item) {
        const auto& text = events_.back().at("text").get_ref<const std::string&>();
        if (has_pickup_continuation(item) && std::ranges::none_of(
            std::array{"这个","这块","这根","那个","那块","那根","原木","左边","右边","this","that","left","right","log"},
            [&](std::string_view word) { return text.find(word) != text.npos; })) return std::nullopt;
        const auto original = read_dig_intent(question_->origin);
        if (!original.material.empty()) intent = original;
    }
    if (!task_.bind_dig(message,intent,args)) return std::nullopt;
    return true;
}

// 阻止用无指向的采料入口绕过明确的单格指令。
bool PartnerLoop::requires_pointed_dig(std::uint64_t session) const {
    return session == session_ && !active_delegation_ && !events_.empty() && events_.back().at("type") == "player.message" &&
        read_dig_intent(events_.back().at("text").get_ref<const std::string&>()).pointed;
}

// 只为原局内事项保留已经登记的拾取后继。
bool PartnerLoop::has_pickup_continuation(std::uint64_t item) const noexcept {
    return item && question_ && item == question_->item && !question_->after_pickup.is_null();
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
    const auto status = question_events_->emit(view(type),type == receipt_type ? 2 : 1,question_sequence_+1,view(wire),&receipt,nullptr);
    if (status == DFY_V2_RESOURCE_EXHAUSTED) return;
    if (status || receipt.sequence != question_sequence_+1) throw std::runtime_error("MC question: event rejected");
    if (type == help_type) {
        question_->event_domain = receipt.domain;
        question_->event_sequence = receipt.sequence;
    }
    ++question_sequence_;
    question_outbound_.pop_front();
}

// 用选定的唯一路径推进实际呈现及可信玩家答复。
// 步骤：结算原发言 → 核对世界与本地时限 → 消费答复或推进既定路径。
void PartnerLoop::advance_question(const BridgeConfig& config, std::uint64_t instance, bool closing) {
    const bool live = !closing && question_ && question_->world == feed_.world() && question_->player == feed_.owner() &&
        !feed_.departed() && (question_->path != "local" || question_->task_revision == task_.revision());
    if (question_work_) {
        std::optional<GameResult> result;
        try { result = question_work_->poll(!live,false,!feed_.recovering()); }
        catch (const std::exception&) { result = GameResult{false,GameEffect::unknown,{}}; }
        if (!result) return;
        question_work_.reset();
        if (live) {
            auto& question = *question_;
            const auto receipt = Json::parse(result->payload,nullptr,false);
            question.presented = result->success && receipt.is_object() && receipt.value("sent",false);
            if (question.path == "local" && !question.presented) { complete_question("unavailable",{}); return; }
            if (question.path == "local") {
                remember_dialogue("companion",question.order,question.text);
                publish("mc.partner.question",{{"status","presented"},{"generation",question.world},
                    {"order",question.order},{"item",question.item},{"text",question.text},
                    {"bridgeWork","ask."+std::to_string(instance)+".l."+std::to_string(question.item)}});
            }
            if (question.ask) question_outbound_.emplace_back(feedback_type,Json{{"ask",question.ask},{"channel","game"},
                {"action","presented"},{"delivered",question.presented},{"recipient",question.player},{"answer",""},
                {"bridgeWork","ask."+std::to_string(instance)+".r."+std::to_string(question.ask)}});
        }
    }
    if (!live) { question_.reset(); flush_question_events(); return; }
    if (feed_.recovering()) { flush_question_events(); return; }
    auto& q = *question_;
    const bool local = q.path == "local";
    if (local && !q.expired && std::chrono::steady_clock::now() >= q.local_answer_deadline) {
        q.expired = true;
        task_.disarm();
        publish("mc.partner.question",{{"status","expired"},{"generation",q.world},{"order",q.order},{"item",q.item}});
    }
    if (q.presented && !q.forwarded) {
        const auto prefix = "答复 "+std::to_string(q.ask ? q.ask : q.item)+" ";
        const auto token = std::to_string(q.ask);
        for (const auto& event : feed_.pending()) {
            const auto text = event.value("text",std::string{});
            if (event.at("sequence") <= q.order) continue;
            std::string answer;
            if (local && q.after_pickup.is_null() && !q.expired && std::ranges::find(q.options,Json(text)) != q.options.end())
                answer = text;
            else if (q.approval && (text == "确认 "+token || text == "approve "+token)) answer = "approve";
            else if (q.approval && (text == "拒绝 "+token || text == "deny "+token)) answer = "deny";
            else if (!local && text.starts_with(prefix)) answer = text.substr(prefix.size());
            if (answer.empty() || (q.approval && answer != "approve" && answer != "deny")) continue;
            q.answer_order = event.at("sequence");
            if (local) { complete_question("answered",answer); return; }
            question_outbound_.emplace_back(feedback_type,Json{{"ask",q.ask},{"channel","game"},{"action","answer"},
                {"delivered",false},{"recipient",q.player},{"answer",answer},{"bridgeWork",""}});
            q.forwarded = true;
            break;
        }
    }
    if (!local && !q.dispatched) {
        Json context = Json::object();
        if (q.approval) context = {{"generation",q.world},{"order",q.order},{"skill",confirmation_->skill},
            {"arguments",confirmation_->arguments}};
        question_outbound_.emplace_back(help_type,Json{{"item",q.item},{"revision",1},{"question",q.text},
            {"reason",q.approval ? "action_approval" : "clarification"},{"context",context.dump()},
            {"path",q.path},{"recipient",q.player},{"channel","game"},{"waitMs",24000}});
        q.dispatched = true;
    }
    const bool start = !q.started && (local || q.ask) && feed_.ready();
    if (start) {
        if (q.approval && confirmation_ && !confirmation_->work && feed_.cursor() != q.order) {
            question_outbound_.emplace_back(feedback_type,Json{{"ask",q.ask},{"channel","game"},
                {"action","presented"},{"delivered",false},{"recipient",q.player},{"answer",""},{"bridgeWork",""}});
            q.started = true;
            flush_question_events();
            return;
        }
        const auto token = std::to_string(q.ask ? q.ask : q.item);
        const auto prompt = local ? q.text : q.text+
            (q.approval ? " 回复 确认 "+token+" 或 拒绝 "+token : " 回复 答复 "+token+" <回答>");
        const auto arguments = Json{{"context",{{"generation",q.world},{"order",feed_.cursor()}}},
            {"arguments",{{"text",prompt}}}}.dump();
        question_work_ = std::make_unique<GameWork>(GameAction::partner,config,
            "ask."+std::to_string(instance)+"."+(q.ask ? "r." : "l.")+token,arguments,"reply");
        q.started = true;
    }
    flush_question_events();
}
}
