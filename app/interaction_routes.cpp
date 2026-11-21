#include "interaction_routes.hpp"
#include "run_log.hpp"
#include <dafeiyu/routed_event_abi_v2.h>
#include <nlohmann/json.hpp>

namespace dfy::app {
namespace {
using Json = nlohmann::json;
// 读取关联身份，拒绝负数、浮点和空身份。
std::uint64_t identity(const Json& value) {
    if (!value.is_number_unsigned() || value == 0) throw std::invalid_argument("interaction: identity required");
    return value.get<std::uint64_t>();
}
}

// 固定事件调度、地址来源和 ASK 时钟。
InteractionRoutes::InteractionRoutes(plugins::PluginManager& plugins, plugins::PluginMailboxHost mail, agent::ToolClock clock,
    std::shared_ptr<RunLog> log)
    : plugins_(plugins), mail_(std::move(mail)), clock_(std::move(clock)), log_(std::move(log)), asks_(clock_) {}

// 取得原始代次仍活动且地址仍绑定的插件信箱。
events::MailboxId InteractionRoutes::destination(const plugins::RoutedEvent& event) const {
    try {
        const auto instance = plugins_.instance(event.plugin);
        if (instance->generation() == event.owner && instance->phase() == plugins::InstancePhase::active)
            return {instance->mailbox_addresses().inbound};
    } catch (const std::invalid_argument&) {}
    return {};
}

// 按解析结果固定原请求或推进原渠道回执。
// 步骤：记录来源及解析 → 核对当前地址 → 分派反馈或接纳提问。
void InteractionRoutes::accept(plugins::RoutedEvent event) {
    if (log_) log_->append("event.decoded",{{"eventDomain",event.id.domain},{"event",event.id.sequence},{"plugin",event.plugin},
        {"instance",event.owner},{"type",event.type},{"status",event.status},{"route",event.kind},{"body",event.payload}});
    if (event.status || !destination(event).value) return;
    const auto body = Json::parse(event.projection);
    if (event.kind == DFY_V2_EVENT_ASK_FEEDBACK) {
        const auto ask = identity(body.at("ask"));
        const auto channel = body.at("channel").get<std::string>();
        const auto action = body.at("action").get<std::string>();
        bool accepted = false;
        if (action == "presented") accepted = asks_.presented(ask,event.owner,channel,body.at("delivered").get<bool>());
        else if (action == "answer") accepted = asks_.answer(ask,event.owner,channel,body.at("recipient"),body.at("answer"));
        if (log_) log_->append("ask."+action,{{"ask",ask},{"instance",event.owner},{"accepted",accepted},{"body",body}});
        return;
    }
    if (event.kind != DFY_V2_EVENT_ASK) return;
    const auto item = identity(body.at("item")), revision = identity(body.at("revision"));
    const auto milliseconds = identity(body.at("waitMs"));
    if (milliseconds > 120000 || body.at("channel") != "game" || exchanges_.size() == 16)
        throw std::invalid_argument("interaction: invalid ASK route");
    exchanges_.push_back({std::move(event),item,revision});
    auto& exchange = exchanges_.back();
    try {
        exchange.ask = asks_.open(exchange.origin.owner,{body.at("question"),body.at("recipient"),body.at("channel"),
            body.value("choices",std::vector<std::string>{}),clock_()+std::chrono::milliseconds(milliseconds)});
    } catch (...) { exchanges_.pop_back(); throw; }
    if (log_) log_->append("ask.opened",{{"ask",exchange.ask},{"eventDomain",exchange.origin.id.domain},
        {"event",exchange.origin.id.sequence},{"item",item},{"revision",revision},{"path","direct"},{"question",body.at("question")}});
}

// 投递有效呈现或原终态，容量不足时保留同一票据重试。
// 步骤：校验原实例 → 构造通用信件 → 成功接纳后推进状态。
bool InteractionRoutes::advance_exchange(Exchange& exchange) {
    const auto* request = asks_.status(exchange.ask);
    const auto inbox = destination(exchange.origin);
    if (!inbox.value) {
        asks_.cancel(exchange.ask,exchange.origin.owner);
        asks_.release(exchange.ask);
        return true;
    }
    const bool pending = request->phase == AskPhase::presenting || request->phase == AskPhase::waiting;
    if (pending && exchange.sent) return false;
    const char* category = pending ? "interaction.ask.present" : "interaction.ask.result";
    Json message{{"ask",exchange.ask},{"item",exchange.item},{"revision",exchange.revision},{"recipient",request->spec.recipient}};
    if (pending) { message["question"] = request->spec.question; message["choices"] = request->spec.choices; }
    else { message["status"] = ask_phase(request->phase); message["answer"] = request->answer; }
    try {
        const auto receipt = mail_.network->deliver_next(mail_.identity,inbox,
            events::mailbox_text(category,1,message.dump(),mail_.network->payload_limit()));
        if (log_) log_->append("interaction.delivered",{{"ask",exchange.ask},{"eventDomain",exchange.origin.id.domain},
            {"event",exchange.origin.id.sequence},{"item",exchange.item},{"category",category},{"mailbox",inbox.value},
            {"message",receipt.event.sequence},{"body",message}});
    } catch (const std::length_error&) { return false; }
    exchange.sent = true;
    if (!pending) asks_.release(exchange.ask);
    return !pending;
}

// 单步接收事件并推进所有已接纳的原提问。
void InteractionRoutes::advance() {
    asks_.advance();
    if (auto event = plugins_.take_routed_event()) {
        const auto id = event->id;
        try { accept(std::move(*event)); }
        catch (const std::exception&) {
            if (log_) log_->append("event.rejected",{{"eventDomain",id.domain},{"event",id.sequence}});
        }
    }
    std::erase_if(exchanges_,[&](auto& exchange) { return advance_exchange(exchange); });
}
}
