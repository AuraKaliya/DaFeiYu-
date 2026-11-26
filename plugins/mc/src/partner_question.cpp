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
// 步骤：验证渠道及问题 → 核对影响范围、后继能力或放置选项 → 按原话和任务修订登记。
nlohmann::json PartnerLoop::open_question(std::uint64_t id, const Json& args) {
    if (!id) throw std::invalid_argument("MC question: unavailable");
    const auto text = args.at("question").get<std::string>(), path = args.value("path",std::string("local"));
    auto options = args.value("options",Json::array());
    if (text.empty() || text.size() > 400 || !options.is_array() || options.size() > 4 ||
        (path != "local" && (!question_events_ || (path != "direct" && path != "main"))))
        throw std::invalid_argument("MC question: invalid request");
    for (const auto& option : options) if (!option.is_string() || option.get_ref<const std::string&>().empty() ||
        option.get_ref<const std::string&>().size() > 160) throw std::invalid_argument("MC question: invalid option");
    if (path == "local" && (chat_line_bytes(text) != text.size() || text.find_first_of("\r\n\0",0,3) != text.npos))
        return {{"status","unpresentable"},{"reason","question_requires_one_chat_line"}};
    const auto& source = events_.empty() ? recent_events_ : events_;
    const Json* original = nullptr;
    if (const auto ref = args.find("sourceMessage"); ref != args.end()) {
        const auto event = std::ranges::find_if(events_,[&](const auto& entry) {
            return entry.at("type") == "player.message" && entry.at("sequence") == *ref;
        });
        if (!ref->is_number_unsigned() || event == events_.end()) return {{"status","invalid_source_message"}};
        original = &*event;
    } else if (!source.empty() && source.back().at("type") == "player.message" && source.back().at("sequence") == order_) {
        original = &source.back();
    }

    const auto after_pickup = args.value("afterPickup",Json(nullptr));
    auto scope = args.value("scope",Json(nullptr));
    if (!scope.is_null()) {
        const auto& skill = scope.at("skill").get_ref<const std::string&>();
        if (path != "local" || args.contains("placementOptions") || (!after_pickup.is_null() && skill != "collect") ||
            (skill != "collect" && skill != "place" && skill != "craft" && skill != "give_item" && skill != "withdraw_item") ||
            (skill == "place" && scope.contains("item")) || ((skill == "craft" || skill == "give_item" || skill == "withdraw_item") && scope.contains("block")))
            throw std::invalid_argument("MC question: invalid action scope");
        for (const auto key : {"block","item"}) if (scope.contains(key)) {
            const auto canonical = item_name(scope.at(key).get_ref<const std::string&>());
            if (canonical.empty()) throw std::invalid_argument("MC question: invalid scope material");
            scope[key] = std::string(canonical);
        }
    }
    if (args.contains("afterPickup") && (path != "local" || events_.empty() || !original ||
        !after_pickup.is_object() ||
        after_pickup.size() != 2+after_pickup.count("pickup") ||
        item_name(after_pickup.at("item").get_ref<const std::string&>()).empty() ||
        !after_pickup.at("count").is_number_integer() || after_pickup.at("count") < 1 || after_pickup.at("count") > 32 ||
        (after_pickup.contains("pickup") && item_name(after_pickup.at("pickup").get_ref<const std::string&>()).empty())))
        throw std::invalid_argument("MC question: invalid pickup continuation");
    for (const auto& [item,q] : questions_) {
        if ((path != "local" && q.path != "local") || (q.text == text && q.path == path && q.scope == scope &&
            q.source_order() == (original ? original->at("sequence").get<std::uint64_t>() : order_) &&
            q.task_revision == task_.revision() && q.after_pickup == after_pickup &&
            (args.contains("placementOptions") ? placement_choice_.item == item && args.at("placementOptions") == placement_choice_.options :
                q.options == options))) return {{"status","waiting"},{"item",item}};
    }
    if (questions_.size() >= 8) return {{"status","busy"}};
    if (const auto choices = args.find("placementOptions"); choices != args.end()) {
        if (placement_choice_pending() && questions_.contains(placement_choice_.item))
            return {{"status","waiting"},{"item",placement_choice_.item}};
        const bool current = placement_choice_.revision == task_.revision();
        if (path != "local" || !after_pickup.is_null() || args.contains("options") || !task_.target_ready() ||
            !choices->is_array() || choices->size() < 2 || choices->size() > 4 ||
            (placement_choice_pending() && *choices != placement_choice_.options))
            throw std::invalid_argument("MC question: invalid placement choices");
        Json used = Json::array();
        for (const auto& option : *choices) {
            const auto& answer = option.at("answer");
            const auto& cells = option.at("cells");
            if (option.size() != 2 || !answer.is_string() || answer.get_ref<const std::string&>().empty() ||
                answer.get_ref<const std::string&>().size() > 160 || std::ranges::find(options,answer) != options.end() ||
                !cells.is_array() || cells.empty() || cells.size() > task_.target_cells().size())
                throw std::invalid_argument("MC question: invalid placement option");
            for (const auto& cell : cells) {
                if (std::ranges::find(task_.target_cells(),cell) == task_.target_cells().end() ||
                    std::ranges::find(used,cell) != used.end() ||
                    (current && std::ranges::any_of(placement_choice_.rejected,[&](const auto& rejected) {
                        return rejected.at("cell") == cell;
                    }))) throw std::invalid_argument("MC question: placement option outside the available task cells");
                used.push_back(cell);
            }
            options.push_back(answer);
        }
        if (!current) placement_choice_ = {};
        placement_choice_.revision = task_.revision();
        placement_choice_.item = id;
        placement_choice_.options = *choices;
    }

    Question question{id,order_,0,session_world_,feed_.owner(),text,path,std::chrono::steady_clock::now()+std::chrono::seconds(40)};
    question.options = std::move(options);
    question.after_pickup = after_pickup;
    question.scope = std::move(scope);
    question.task_revision = task_.revision();
    if (original) question.origin = *original;
    if (!question.scope.is_null() && question.scope.at("skill") == "place") task_.pin_opening_observation(question.source_order());
    if (path == "local") for (auto& [item,q] : questions_)
        if (q.path == "local") q.numbered = question.numbered = true;
    questions_.emplace(id,std::move(question));
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
    auto* question = questions_.contains(item) ? &questions_.at(item) : nullptr;
    const bool matched = question && domain == question->event_domain && event == question->event_sequence &&
        (!question->ask || question->ask == id) && body.value("recipient",std::string{}) == question->player &&
        question->world == feed_.world() && question->player == feed_.owner() && !feed_.departed() && !feed_.departure();
    if (category == "interaction.ask.present") {
        if (matched && id && !question->ask && body.value("question",std::string{}) == question->text) {
            question->ask = id;
            question->presented = false;
        }
        return;
    }
    const bool current = matched && (!question->approval || (confirmation_ && confirmation_->request == item &&
        confirmation_->decision.is_null() && std::chrono::steady_clock::now() < confirmation_->deadline &&
        confirmation_context_current(body.at("channel") == "game" ? question->answer_order : question->order)));
    if (matched)
        complete_question(item,body.at("status").get<std::string>(),body.at("answer").get<std::string>(),body.at("channel").get<std::string>());
    question_outbound_.emplace_back(receipt_type,Json{{"ask",id},{"item",item},{"revision",1},{"eventDomain",domain},
        {"event",event},{"disposition",current ? "matched" : "stale"},{"mailbox",message.mailbox},{"message",message.sequence}});
}

// 结算原问题并将答复及获选方格交给后续回合。
// 步骤：分流原审批 → 记录匹配答复及被排除的方格 → 释放问题并安排续行。
void PartnerLoop::complete_question(std::uint64_t item, std::string_view status, std::string answer, std::string_view channel) {
    const auto& question = questions_.at(item);
    if (question.approval) {
        if (confirmation_ && confirmation_->request == question.item) {
            const auto order = channel == "game" ? question.answer_order : question.order;
            const auto source = channel == "game" ? "game" : "main";
            if (std::chrono::steady_clock::now() >= confirmation_->deadline) confirm_once("expired","host");
            else if (status == "answered" && answer == "approve" && confirmation_context_current(order)) confirm_once("approved",source,order);
            else if (status == "answered" && answer == "deny") confirm_once("denied",source);
            else if (status == "expired") confirm_once("expired","host");
            else confirm_once("withdrawn","host");
        }
        questions_.erase(item);
        return;
    }
    auto& result = question_results_.emplace_back(Json{{"item",item},{"question",question.text},{"status",status},
        {"answer",std::move(answer)},{"player",question.player},{"taskRevision",question.task_revision}});
    if (!question.origin.is_null()) result["originalText"] = question.origin.at("text");
    if (question.answer_order) result["answerMessage"] = question.answer_order;
    if (!question.scope.is_null()) result["scope"] = question.scope;
    if (!question.scope.is_null() || !question.origin.is_null()) result["sourceMessage"] = question.source_order();
    if (status == "answered" && placement_choice_pending() && placement_choice_.item == question.item) {
        for (const auto& option : placement_choice_.options) {
            if (option.at("answer") == result.at("answer")) result["placementCells"] = option.at("cells");
            else for (const auto& cell : option.at("cells"))
                placement_choice_.rejected.push_back({{"cell",cell},{"item",question.item}});
        }
        placement_choice_.options.clear();
    }
    questions_.erase(item);
    question_wake_ |= status != "resolved" && status != "superseded";
}

// 切换任务撤销旧问题，纠正只撤销早于本条原话的局内事项。
void PartnerLoop::supersede_questions(bool correcting) {
    for (auto it = questions_.begin(); it != questions_.end();) {
        const auto& q = (it++)->second;
        if (!q.approval && (!correcting || (q.path == "local" && q.order < order_))) complete_question(q.item,"superseded",{});
    }
}

// 核对事项仍属于当前世界、玩家和原任务修订。
const PartnerLoop::Question* PartnerLoop::waiting_question(std::uint64_t session, std::uint64_t item) const noexcept {
    const auto it = questions_.find(item);
    if (session != session_ || active_delegation_ || it == questions_.end()) return nullptr;
    const auto& q = it->second;
    return q.path == "local" && !q.approval && q.world == feed_.world() && q.player == feed_.owner() &&
        q.task_revision == task_.revision() ? &q : nullptr;
}

// 判断候选是否依赖该问题，材料未固定时保留可能的重叠。
// 步骤：匹配执行器 → 核对已知的材料限制。
bool PartnerLoop::question_covers(const Json& scope, std::string_view skill, const Json& args) {
    if (scope.is_null()) return true;
    if (scope.at("skill").get_ref<const std::string&>() != (skill == "fill_target" ? "place" : skill)) return false;
    for (const auto key : {"block","item"}) if (scope.contains(key) && args.contains(key) &&
        scope.at(key).get_ref<const std::string&>() != item_name(args.at(key).get_ref<const std::string&>())) return false;
    return true;
}

// 放置选择在实际答复后解除等待，未获选的格仍受原任务修订约束。
bool PartnerLoop::placement_choice_pending() const noexcept {
    return placement_choice_.revision == task_.revision() && !placement_choice_.options.empty();
}

// 按实际写入范围核对步骤是否需要玩家答复。
// 步骤：按执行器及材料核对追问 → 核对未获选的放置格 → 比对待答方格及其支撑。
Json PartnerLoop::action_question(std::uint64_t session, std::string_view skill, const Json& args, std::uint64_t resolves) const {
    // 存箱与投物共用交付去向待答，答复只解除对应材料的交付等待。
    if (skill == "store_item") skill = "give_item";
    const bool placing = skill == "place" || skill == "fill_target";
    if ((!placing && skill != "collect" && skill != "craft" && skill != "give_item" && skill != "withdraw_item") || session != session_ || !session || delegated(session)) return {};
    const auto blocked = [](std::uint64_t item, std::string_view reason) {
        return Json{{"status","blocked"},{"reason",reason},{"item",item}};
    };
    const bool pending = placement_choice_pending();
    for (const auto& [item,q] : questions_) {
        if (((skill != "give_item" && skill != "withdraw_item") || !q.scope.is_null()) && item != resolves && waiting_question(session,item) && (!pending || item != placement_choice_.item) &&
            question_covers(q.scope,skill,args)) return blocked(item,"waiting_for_player");
    }
    if (skill == "craft" || skill == "give_item" || skill == "withdraw_item" || placement_choice_.revision != task_.revision()) return {};
    const bool fixed_cell = valid_action_cell(args);
    // 无固定格的地面取材由已绑定的保护格及实际地形筛选。
    if (skill == "collect" && !fixed_cell && args.value("source","") == "ground") return {};

    const auto touches = [&](const Json& cell) {
        if (skill == "fill_target") return task_.target_ready() &&
            std::ranges::find(task_.target_cells(),cell) != task_.target_cells().end();
        if (!fixed_cell) return true;
        if (cell.at("x") != args.at("x") || cell.at("z") != args.at("z")) return false;
        const auto dy = cell.at("y").get<int>() - args.at("y").get<int>();
        const bool door = args.value("block","").ends_with("_door");
        if (placing) return dy == 0 || (door && dy == 1);
        return dy == 0 || dy == 1 || (door && (dy == -1 || dy == 2));
    };
    if (placing) for (const auto& rejected : placement_choice_.rejected)
        if (touches(rejected.at("cell"))) return blocked(rejected.at("item").get<std::uint64_t>(),"answer_target_mismatch");
    if (pending && std::ranges::any_of(placement_choice_.options,[&](const auto& option) {
        return std::ranges::any_of(option.at("cells"),touches);
    })) return blocked(placement_choice_.item,"waiting_for_player");
    return {};
}

// 只有当前玩家提供了新指向且同轮观察支持原动作时才注销追问。
// 步骤：核对事项和玩家原话 → 用可信观察匹配动作 → 记录无需唤醒的结算。
void PartnerLoop::resolve_block_question(std::uint64_t item, std::string_view envelope) {
    const auto* q = waiting_question(session_,item);
    if (!q || order_ <= q->order || events_.empty() || events_.back().at("type") != "player.message") return;
    auto args = Json::parse(envelope).at("arguments");
    if (!pointed_dig(session_,order_,item,args).value_or(false)) return;
    complete_pointed_question(item);
}

// 把解开的自然追问与提供新指向的原话绑定。
void PartnerLoop::complete_pointed_question(std::uint64_t item) {
    questions_.at(item).answer_order = order_;
    complete_question(item,"resolved",events_.back().at("text").get<std::string>());
}

// 按原话和动作引用判定是否需要校验玩家指向。
// 步骤：识别指向要求 → 核对原话和待答事项 → 匹配坐标和材质。
std::optional<bool> PartnerLoop::pointed_dig(std::uint64_t session, std::uint64_t message, std::uint64_t item,
    Json& args) const {
    if (session != session_ || active_delegation_) return !message && !item ? std::optional(false) : std::nullopt;
    const bool player_event = !events_.empty() && events_.back().at("type") == "player.message";
    auto intent = player_event ? read_dig_intent(events_.back().at("text").get_ref<const std::string&>()) : DigIntent{};
    if (!message && !item && !intent.pointed) return false;
    if (!message) message = order_;
    const auto* question = waiting_question(session,item);
    if (message != order_ || !player_event ||
        (item && (!question || (placement_choice_pending() && placement_choice_.item == item) ||
            !question->presented || question->expired || std::chrono::steady_clock::now() >= question->local_answer_deadline ||
            question->presentation_order >= order_ || (question->shown_with_number &&
                !events_.back().at("text").get_ref<const std::string&>().starts_with(question->answer_prefix())) ||
            !question_covers(question->scope,"collect",args)))) return std::nullopt;
    if (item) {
        const auto& text = events_.back().at("text").get_ref<const std::string&>();
        if (has_pickup_continuation(item) && std::ranges::none_of(
            std::array{"这个","这块","这根","那个","那块","那根","原木","左边","右边","this","that","left","right","log"},
            [&](std::string_view word) { return text.find(word) != text.npos; })) return std::nullopt;
        if (!question->origin.is_null()) {
            const auto original = read_dig_intent(question->origin.at("text").get_ref<const std::string&>());
            if (!original.material.empty()) intent = original;
        }
    }
    if (!task_.bind_dig(message,intent,args)) return std::nullopt;
    return true;
}

// 核对自选目标引用的自身读回，并移除已由宿主校验的引用。
std::string_view PartnerLoop::check_dig_observation(std::uint64_t session, Json& args) const {
    if (!args.contains("observation")) return {};
    const auto issue = session == session_ && !active_delegation_ ? task_.dig_observation_issue(args,feed_.observation()) : "observation_stale";
    args.erase("observation");
    return issue;
}

// 阻止用无指向的采料入口绕过明确的单格指令。
bool PartnerLoop::requires_pointed_dig(std::uint64_t session) const {
    return session == session_ && !active_delegation_ && !events_.empty() && events_.back().at("type") == "player.message" &&
        read_dig_intent(events_.back().at("text").get_ref<const std::string&>()).pointed;
}

// 只为原局内事项保留已经登记的拾取后继。
bool PartnerLoop::has_pickup_continuation(std::uint64_t item) const noexcept {
    const auto* q = waiting_question(session_,item);
    return q && !q->after_pickup.is_null();
}

// 跳过已退役的原求助，宿主容量暂满时保留原事件及发送序号。
// 步骤：核对队首求助 → 提交并保留暂满项 → 记录接纳身份并出队。
void PartnerLoop::flush_question_events() {
    while (!question_outbound_.empty() && question_outbound_.front().first == help_type &&
        !questions_.contains(question_outbound_.front().second.at("item").get<std::uint64_t>())) question_outbound_.pop_front();
    if (question_outbound_.empty()) return;
    const auto& [type,body] = question_outbound_.front();
    const auto wire = body.dump();
    dfy_v2_event_receipt receipt{};
    const auto status = question_events_->emit(view(type),type == receipt_type ? 2 : 1,question_sequence_+1,view(wire),&receipt,nullptr);
    if (status == DFY_V2_RESOURCE_EXHAUSTED) return;
    if (status || receipt.sequence != question_sequence_+1) throw std::runtime_error("MC question: event rejected");
    if (type == help_type) {
        auto& q = questions_.at(body.at("item").get<std::uint64_t>());
        q.event_domain = receipt.domain;
        q.event_sequence = receipt.sequence;
    }
    ++question_sequence_;
    question_outbound_.pop_front();
}

// 用选定的唯一路径推进实际呈现及可信玩家答复。
// 步骤：结算原发言 → 核对世界与本地时限 → 消费答复或推进既定路径。
void PartnerLoop::advance_question(const BridgeConfig& config, std::uint64_t instance, bool closing) {
    const auto live = [&](const Question& q) {
        return !closing && q.world == feed_.world() && q.player == feed_.owner() && !feed_.departed() && !feed_.departure() &&
            (q.path != "local" || q.task_revision == task_.revision());
    };
    if (question_work_) {
        const auto found = questions_.find(question_work_item_);
        const bool current = found != questions_.end() && live(found->second);
        std::optional<GameResult> result;
        const auto order = found == questions_.end() ? order_ : found->second.source_order();
        try { result = poll_game(*question_work_,session_,!current,false,!feed_.recovering(),order); }
        catch (const std::exception&) { result = GameResult{false,GameEffect::unknown,{}}; }
        if (!result) return;
        settle_presentation(question_work_,order,*result);
        question_work_.reset();
        if (current) {
            auto& question = found->second;
            const auto receipt = Json::parse(result->payload,nullptr,false);
            const bool sent = result->success && receipt.is_object() && receipt.value("sent",false);
            if (question.path == "local" && !sent) {
                complete_question(question.item,"unavailable",{});
                auto& failed = question_results_.back();
                failed["bridgeWork"] = question_delivery_;
                failed["delivery"] = result->effect == GameEffect::none ? "not_sent" : "unknown";
                return;
            }
            if (sent) for (const auto item : question_delivery_numbers_)
                if (auto q = questions_.find(item); q != questions_.end()) q->second.shown_with_number = true;
            question.presented = sent && !question_delivery_hint_;
            if (question.path == "local") {
                const Json origin{{"sourceMessage",question.source_order()},{"questionItem",question.item}};
                remember_dialogue("companion",question.order,receipt.at("text"),origin);
                publish("mc.partner.question",{{"status",question_delivery_hint_ ? "number_presented" : "presented"},{"generation",question.world},
                    {"order",question.order},{"sourceMessage",question.source_order()},{"item",question.item},{"text",receipt.at("text")},
                    {"bridgeWork",question_delivery_}});
                if (question_delivery_hint_) { question.hint_sent = true; question.started = false; }
            }
            if (question.ask) question_outbound_.emplace_back(feedback_type,Json{{"ask",question.ask},{"channel","game"},
                {"action","presented"},{"delivered",question.presented},{"recipient",question.player},{"answer",""},
                {"bridgeWork","ask."+std::to_string(instance)+".r."+std::to_string(question.ask)}});
        }
    }
    for (auto it = questions_.begin(); it != questions_.end();) {
        auto current = it++;
        if (!live(current->second)) { questions_.erase(current); continue; }
        if (!feed_.recovering()) advance_question_item(current->second,config,instance);
    }
    if (!feed_.recovering()) for (const auto& event : feed_.pending()) {
        if (event.at("type") != "player.message") continue;
        for (auto it = questions_.begin(); it != questions_.end(); ++it)
            if (accept_question_answer(it->second,event)) break;
    }
    flush_question_events();
}

// 按玩家协议原话的顺序匹配事项，带编号的答复只可消费一次。
// 步骤：核对呈现水位与目标编号 → 校验选项或审批词 → 结算局内事项或投递原 ASK 回执。
bool PartnerLoop::accept_question_answer(Question& q, const Json& event) {
    if (!q.presented || q.forwarded || event.at("sequence") <= std::max(q.order,q.presentation_order)) return false;
    const auto& text = event.at("text").get_ref<const std::string&>();
    const auto prefix = q.answer_prefix();
    std::string answer;
    if (q.path == "local") {
        if (!q.after_pickup.is_null() || q.expired) return false;
        const bool addressed = text.starts_with(prefix);
        if (!addressed && q.shown_with_number) return false;
        answer = addressed ? text.substr(prefix.size()) : text;
        if (answer.empty() || ((!addressed || !q.options.empty()) && std::ranges::find(q.options,Json(answer)) == q.options.end()))
            return false;
        q.answer_order = event.at("sequence");
        complete_question(q.item,"answered",std::move(answer));
        return true;
    }
    const auto token = std::to_string(q.ask);
    if (q.approval && (text == "确认 "+token || text == "approve "+token)) answer = "approve";
    else if (q.approval && (text == "拒绝 "+token || text == "deny "+token)) answer = "deny";
    else if (text.starts_with(prefix)) answer = text.substr(prefix.size());
    if (answer.empty() || (q.approval && answer != "approve" && answer != "deny")) return false;
    q.answer_order = event.at("sequence");
    question_outbound_.emplace_back(feedback_type,Json{{"ask",q.ask},{"channel","game"},{"action","answer"},
        {"delivered",false},{"recipient",q.player},{"answer",answer},{"bridgeWork",""}});
    q.forwarded = true;
    return true;
}

// 推进单个事项的期限及呈现，共用发言读口保留原事项身份。
// 步骤：标记超时 → 投递路由请求 → 按既定渠道发出问题。
void PartnerLoop::advance_question_item(Question& q, const BridgeConfig& config, std::uint64_t instance) {
    const bool local = q.path == "local";
    if (local && !q.expired && std::chrono::steady_clock::now() >= q.local_answer_deadline) {
        q.expired = true;
        if (q.scope.is_null() && (!placement_choice_pending() || placement_choice_.item != q.item)) task_.disarm();
        publish("mc.partner.question",{{"status","expired"},{"generation",q.world},{"order",q.order},{"item",q.item}});
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
    const bool start = !q.started && !question_work_ && (local || q.ask) && feed_.ready();
    if (start) {
        if (q.approval && confirmation_ && !confirmation_->work && feed_.cursor() != q.order) {
            question_outbound_.emplace_back(feedback_type,Json{{"ask",q.ask},{"channel","game"},
                {"action","presented"},{"delivered",false},{"recipient",q.player},{"answer",""},{"bridgeWork",""}});
            q.started = true;
            return;
        }
        const auto token = std::to_string(q.ask ? q.ask : q.item);
        auto prompt = q.text;
        std::vector<std::uint64_t> numbers;
        bool separate_hint = false;
        if (!local || (q.numbered && !q.hint_sent)) {
            auto hint = q.approval ? "回复 确认 "+token+" 或 拒绝 "+token : "回复 "+q.answer_prefix()+"<回答>";
            if (local) numbers.push_back(q.item);
            if (local) for (const auto& [item,other] : questions_)
                if (item != q.item && other.path == "local" && other.presented && !other.shown_with_number) {
                    hint += "（上一问编号 "+std::to_string(item)+"）";
                    numbers.push_back(item);
                }
            prompt += " "+hint;
            separate_hint = local && chat_line_bytes(prompt) != prompt.size();
            if (separate_hint) prompt = "下一问请"+hint;
        }
        const auto arguments = Json{{"context",{{"generation",q.world},{"order",feed_.cursor()}}},
            {"arguments",{{"text",prompt}}}}.dump();
        question_delivery_ = "ask."+std::to_string(instance)+"."+(q.ask ? "r." : "l.")+token+(separate_hint ? ".number" : "");
        question_work_ = std::make_unique<GameWork>(GameAction::partner,config,question_delivery_,arguments,"reply");
        question_delivery_hint_ = separate_hint;
        question_delivery_numbers_ = std::move(numbers);
        question_work_item_ = q.item;
        q.presentation_order = feed_.cursor();
        q.started = true;
    }
}
}
