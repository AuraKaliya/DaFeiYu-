#include "companion.hpp"
#include <nlohmann/json.hpp>

namespace dfy::app {
namespace {
using Json = nlohmann::json;
// 复制有序游戏事实，名单事件不制造玩家原话。
Json event_payload(const std::deque<PlayerUtterance>& entries) {
    Json events = Json::array();
    for (const auto& entry : entries) {
        Json fact{{"sequence",entry.sequence},{"type",entry.type}};
        if (entry.type == "player.message") fact["text"] = entry.text;
        events.push_back(std::move(fact));
    }
    return events;
}
// 查找这个回合仍实际持有的原生工作，以免迟到的决定中断另一个动作。
std::optional<agent::BatchToolSnapshot> running_skill(const agent::TurnRequest& run) {
    const auto steps = run.tool_steps();
    for (const auto& step : steps)
        if (step.accepted && step.exit == runtime::OperationState::pending) return step;
    return {};
}
}

// 收到新事实时，正在执行的工作继续接受原票据轮询，是否提前停手交给模型。
// 步骤：结算在途判断并核对原工作 → 在工具边界让出下一次决策 → 必要时提交一次停止选择。
void CompanionSession::review_partner_events() {
    const bool pending = partner_order_ && !stopping_ && address_ == partner_order_->address && !inbox_.pending().empty();
    if (pending) *partner_order_->newer = true;
    if (event_check_) {
        event_model_.advance();
        if (event_check_->exit()->state() == runtime::OperationState::pending) return;
        auto completed = std::move(event_check_);
        if (partner_order_ && partner_order_->run && address_ == partner_order_->address && address_->binding == check_binding_ &&
            partner_order_->run->turn() == check_turn_ && inbox_.status().cursor == check_cursor_) {
            const auto step = running_skill(*partner_order_->run);
            if (step && step->candidate == check_candidate_) {
                const char* status = "未取得有效决定";
                if (completed->operation()->state() == runtime::OperationState::succeeded) {
                    const auto& calls = completed->response().message.tool_calls;
                    if (calls.empty()) status = "保留在途工作";
                    else if (calls.size() == 1 && calls.front().name == "stop_current_work" &&
                        Json::parse(calls.front().arguments, nullptr, false) == Json::object()) {
                        partner_order_->stop_requested = conversation_.cancel();
                        status = partner_order_->stop_requested ? "已请求停止" : "停止请求未接纳";
                    }
                }
                // 随原事实一起投影，避免计划板把决定排在玩家原话之前。
                partner_order_->decisions.emplace_back(check_cursor_, "工作 #" + std::to_string(check_candidate_) + " · " + status);
            }
        }
    }
    if (!pending || !partner_order_->run) return;
    auto& order = *partner_order_;
    const auto cursor = inbox_.status().cursor;
    if (!conversation_.busy() || order.reviewed == cursor || order.stop_requested) return;
    const auto step = running_skill(*order.run);
    if (!step) return;

    Json input{{"currentEvents",Json::parse(order.facts)},{"work",{{"skill",step->name},{"candidate",step->candidate},{"state","running"}}},{"newEvents",event_payload(inbox_.pending())},
        {"player",inbox_.status().owner},{"generation",address_->world}};
    if (reader_.state().last) input["lastObservation"] = Json::parse(reader_.state().last->payload);
    Json dialogue = Json::array();
    for (const auto& msg : messages_.since(0).messages) {
        if (msg.address != *address_) continue;
        const bool system = msg.kind == CompanionMessageKind::failed || msg.kind == CompanionMessageKind::status;
        const auto kind = msg.kind == CompanionMessageKind::event ? "game.event" : system ? "system.status" :
            msg.from == CompanionSide::primary ? "input" : "game.reply";
        dialogue.push_back({{"kind",kind},{"text",msg.text}});
    }
    input["recentDialogue"] = std::move(dialogue);
    model::Request request{{{model::Role::system,"你是正在游戏里行动的大肥鱼。结合分工、现场和新事件自行决定是否提前停下这项工作。"
        "只有选择停手才调用 stop_current_work；否则让原动作继续。这个判断不向玩家发言，原事件随后进入对话。"},
        {model::Role::user,input.dump()}},{{"stop_current_work","请求中止这里列出的在途工作，保留已经产生的效果。",
        R"({"type":"object","properties":{},"additionalProperties":false})"}}};
    order.reviewed = cursor;
    check_candidate_ = step->candidate;
    check_turn_ = order.run->turn();
    check_binding_ = address_->binding;
    check_cursor_ = cursor;
    try { event_check_ = event_model_.submit_request(event_config_, std::move(request)); }
    catch (const std::exception&) {
        // 判断不可用时保留原动作，原事件仍在收件队列。
    }
}

// 将玩家在场变化与原话送进持续会话；只将实际发言工具的回执记为角色说话。
// 步骤：等待原工作退出 → 记录真实发送及故障 → 提交本批事实与原始来源。
void CompanionSession::advance_partner() {
    std::vector<std::pair<std::uint64_t, std::string>> decisions;
    if (partner_order_) {
        auto& order = *partner_order_;
        if (conversation_.busy()) return;
        if (!stopping_ && address_ == order.address) {
            if (order.run) for (const auto& step : order.run->tool_steps()) {
                if (step.tool_id != partner_reply_tool_) continue;
                const auto result = order.run->tool_result(step.candidate);
                if (!result || !result->succeeded) continue;
                const auto receipt = Json::parse(result->output, nullptr, false);
                if (receipt.is_object() && receipt.value("sent", false) && !receipt.value("reused", false) && receipt.contains("text"))
                    messages_.post(order.address, CompanionSide::game, CompanionMessageKind::report,
                        messages_.next(CompanionSide::game), order.message, receipt.at("text").get<std::string>());
            }
            if (*order.newer) {
                prior_activity_ = Json{{"events",Json::parse(order.facts)},{"stopRequested",order.stop_requested}}.dump();
                decisions = std::move(order.decisions);
            } else if (!result_ || result_->error) {
                messages_.post(order.address, CompanionSide::game, CompanionMessageKind::failed,
                    messages_.next(CompanionSide::game), order.message, "游戏决策回合未完成");
            }
            settled_event_ = order.order;
        }
        partner_order_.reset();
    }
    if (stopping_ || !address_ || queued_ || delivering_ || conversation_.busy() || inbox_.pending().empty()) return;
    if (reset_partner_history_) { conversation_.clear(); reset_partner_history_ = false; }
    auto facts = event_payload(inbox_.pending());
    std::uint64_t seq = 0, message = 0;
    while (auto entry = inbox_.take()) {
        seq = entry->sequence;
        const bool speech = entry->type == "player.message";
        message = messages_.post(*address_, CompanionSide::primary, speech ? CompanionMessageKind::utterance : CompanionMessageKind::event,
            messages_.next(CompanionSide::primary), 0, speech ? entry->text : entry->type).revision;
        for (const auto& [source, status] : decisions) if (source == seq)
            messages_.post(*address_, CompanionSide::game, CompanionMessageKind::status, messages_.next(CompanionSide::game), message, status);
    }
    Json context{{"generation",address_->world},{"order",seq}};
    Json input{{"player",inbox_.status().owner},{"generation",address_->world},{"events",facts}};
    if (reader_.state().last) input["lastObservation"] = Json::parse(reader_.state().last->payload);
    if (!prior_activity_.empty()) { input["previousActivity"] = Json::parse(prior_activity_); prior_activity_.clear(); }

    partner_order_ = PartnerOrder{*address_,seq,message,facts.dump(),{},std::make_shared<bool>(false)};
    result_.reset();
    try {
        agent::RunOptions options;
        options.now = clock_;
        options.tools = catalog_->snapshot(partner_tools_, true).bind_context(context.dump());
        options.version_labels.prompt_template = "game-partner-events-v1";
        options.system_instructions = "你是大肥鱼，用自己的角色和玩家一起玩。events 是按序收到的在场变化和玩家原话。"
            "结合当前约定、自己的打算和已确认的现场，自行选择行动、协商、拒绝或安静等待。"
            "要对玩家说话时使用回复工具；回合结束的文本是内部结算，不会发到游戏。";
        // 新事实只拦住旧决策继续发起下一步，已经接纳的工作保留真实退出责任。
        options.extension = [newer = partner_order_->newer](agent::ControlPoint point, const agent::PolicyContext&) {
            return *newer && point != agent::ControlPoint::tool_result
                ? agent::PolicyDecision{agent::PolicyVerdict::deny,agent::PolicyReason::stale_goal}
                : agent::PolicyDecision{agent::PolicyVerdict::allow,agent::PolicyReason::none};
        };
        conversation_.submit(input.dump(), std::move(options));
        partner_order_->run = conversation_.current_;
    } catch (const std::exception&) {
        // 下一次推进会把接纳故障登记为系统事实。
    }
}
}
