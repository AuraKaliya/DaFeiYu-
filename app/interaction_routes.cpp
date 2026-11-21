#include "interaction_routes.hpp"
#include "run_log.hpp"
#include <dafeiyu/routed_event_abi_v2.h>
#include <nlohmann/json.hpp>
#include <algorithm>

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
InteractionRoutes::InteractionRoutes(plugins::PluginManager& plugins, plugins::PluginMailboxHost mail, MailboxIntake& intake,
    agent::ToolClock clock, std::shared_ptr<RunLog> log, bool auto_wake)
    : plugins_(plugins), mail_(std::move(mail)), intake_(intake), clock_(std::move(clock)), log_(std::move(log)), asks_(clock_),
      auto_wake_(auto_wake) {}

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
    if (event.kind == DFY_V2_EVENT_MAIL) {
        if (exchanges_.size() == 16) throw std::length_error("interaction: pending route capacity exhausted");
        const auto box = main_box(event);
        exchanges_.push_back({std::move(event),0,0,0,clock_()+std::chrono::seconds(120),box});
        return;
    }
    const bool main = event.kind == DFY_V2_EVENT_ASK_VIA_MAIL;
    if (event.kind != DFY_V2_EVENT_ASK && !main) return;
    const auto item = identity(body.at("item")), revision = identity(body.at("revision"));
    const auto milliseconds = identity(body.at("waitMs"));
    if (milliseconds > 120000 || (body.at("channel") != "game" && body.at("channel") != "terminal") || exchanges_.size() == 16)
        throw std::invalid_argument("interaction: invalid ASK route");
    if (std::ranges::any_of(exchanges_,[&](const auto& other) {
        return other.origin.owner == event.owner && other.item == item && other.revision == revision;
    })) return;
    const auto box = main ? main_box(event) : events::MailboxId{};
    exchanges_.push_back({std::move(event),item,revision,0,clock_()+std::chrono::milliseconds(milliseconds),box});
    auto& exchange = exchanges_.back();
    if (!main) {
        try { open_ask(exchange,body.at("channel")); }
        catch (...) { exchanges_.pop_back(); throw; }
    }
}

// 创建并记录原事项的唯一提问，重试复用同一票据。
void InteractionRoutes::open_ask(Exchange& exchange, std::string channel) {
    if (exchange.ask) {
        if (asks_.status(exchange.ask)->spec.channel != channel) throw std::invalid_argument("interaction: existing ASK uses a different channel");
        return;
    }
    if (channel != "game" && channel != "terminal") throw std::invalid_argument("interaction: unknown channel");
    const auto body = Json::parse(exchange.origin.projection);
    exchange.ask = asks_.open(exchange.origin.owner,{body.at("question"),body.at("recipient"),std::move(channel),
        body.value("choices",std::vector<std::string>{}),exchange.deadline});
    if (log_) log_->append("ask.opened",{{"ask",exchange.ask},{"eventDomain",exchange.origin.id.domain},
        {"event",exchange.origin.id.sequence},{"item",exchange.item},{"revision",exchange.revision},
        {"path",exchange.main_box.value ? "main" : "direct"},{"question",body.at("question")},{"channel",asks_.status(exchange.ask)->spec.channel}});
}

// 投递有效呈现或原终态，容量不足时保留同一票据重试。
// 步骤：校验原实例 → 构造通用信件 → 成功接纳后推进状态。
bool InteractionRoutes::advance_exchange(Exchange& exchange) {
    const auto inbox = destination(exchange.origin);
    if (!inbox.value) {
        asks_.cancel(exchange.ask,exchange.origin.owner);
        asks_.release(exchange.ask);
        return true;
    }
    if (!exchange.ask) {
        if (clock_() >= exchange.deadline) {
            if (log_) log_->append("interaction.expired",{{"eventDomain",exchange.origin.id.domain},{"event",exchange.origin.id.sequence},
                {"item",exchange.item},{"stage",exchange.initial_queued ? "awaiting_ask" : "awaiting_mailbox"}});
            return true;
        }
        if (!exchange.initial_queued) {
            auto message = Json::parse(exchange.origin.projection);
            message["eventDomain"] = exchange.origin.id.domain;
            message["event"] = exchange.origin.id.sequence;
            message["requiresAsk"] = bool(exchange.item);
            const auto& rule = exchange.origin.mailbox;
            try {
                const auto receipt = post(exchange.origin.owner,exchange.main_box,rule.category,rule.version,message.dump());
                exchange.initial_queued = true;
                if (log_) log_->append("interaction.queued",{{"eventDomain",exchange.origin.id.domain},{"event",exchange.origin.id.sequence},
                    {"mailbox",exchange.main_box.value},{"message",receipt.event.sequence},{"item",exchange.item},
                    {"wake",bool(auto_wake_ && (rule.flags & DFY_V2_MAIL_WAKE))}});
            } catch (const std::length_error&) {}
        }
        return !exchange.item && exchange.initial_queued;
    }
    const auto* request = asks_.status(exchange.ask);
    const bool pending = request->phase == AskPhase::presenting || request->phase == AskPhase::waiting;
    if (pending && (exchange.sent || request->spec.channel == "terminal")) return false;
    if (!pending && exchange.main_box.value && clock_() >= exchange.deadline+std::chrono::seconds(5)) {
        asks_.release(exchange.ask);
        return true;
    }
    const bool to_main = !pending && exchange.main_box.value && !exchange.reply_requested;
    if (to_main && exchange.result_queued) return false;
    const char* completion = exchange.origin.mailbox.flags & DFY_V2_MAIL_WAKE ? "interaction.ask.completed" : "interaction.ask.completed.passive";
    const char* category = pending ? "interaction.ask.present" : to_main ? completion :
        exchange.main_box.value ? "interaction.mail.reply" : "interaction.ask.result";
    Json message{{"ask",exchange.ask},{"item",exchange.item},{"revision",exchange.revision},{"recipient",request->spec.recipient},
        {"channel",request->spec.channel},{"requiresAsk",true}};
    if (pending) { message["question"] = request->spec.question; message["choices"] = request->spec.choices; }
    else { message["status"] = ask_phase(request->phase); message["answer"] = request->answer; }
    message["eventDomain"] = exchange.origin.id.domain;
    message["event"] = exchange.origin.id.sequence;
    try {
        const auto box = to_main ? exchange.main_box : inbox;
        const auto receipt = post(to_main ? exchange.origin.owner : mail_.identity,box,category,1,message.dump());
        if (log_) log_->append("interaction.delivered",{{"ask",exchange.ask},{"eventDomain",exchange.origin.id.domain},
            {"event",exchange.origin.id.sequence},{"item",exchange.item},{"category",category},{"mailbox",box.value},
            {"message",receipt.event.sequence},{"body",message}});
    } catch (const std::length_error&) { return false; }
    if (to_main) { exchange.result_queued = true; return false; }
    if (pending) exchange.sent = true;
    if (!pending) asks_.release(exchange.ask);
    return !pending;
}

// 查询应由终端下一次输出的原问题。
std::optional<std::pair<std::uint64_t,AskSpec>> InteractionRoutes::console_question() const {
    for (const auto& exchange : exchanges_) {
        const auto* request = asks_.status(exchange.ask);
        if (request && request->phase == AskPhase::presenting && request->spec.channel == "terminal")
            return std::pair{exchange.ask,request->spec};
    }
    return {};
}

// 从本体终端按原身份确认呈现或接纳一条答复。
bool InteractionRoutes::console_answer(std::uint64_t id, std::optional<std::string> answer) {
    const auto* request = asks_.status(id);
    if (!request || request->spec.channel != "terminal") return false;
    const auto owner = request->owner;
    const bool accepted = answer ? asks_.answer(id,owner,"terminal",request->spec.recipient,*answer) : asks_.presented(id,owner,"terminal",true);
    if (log_) log_->append(answer ? "ask.answer" : "ask.presented",{{"ask",id},{"instance",owner},{"accepted",accepted},{"channel","terminal"},
        {"answer",answer.value_or(std::string{})}});
    return accepted;
}

// 将原件投递到已由宿主授权的地址，并记录独立接纳。
events::DeliveryReceipt InteractionRoutes::post(runtime::DomainId sender, events::MailboxId box, std::string_view category,
    std::uint32_t version, std::string_view wire) {
    const auto result = mail_.network->deliver_next(sender,box,events::mailbox_text(category,version,wire,mail_.network->payload_limit()));
    if (log_ && result.state == events::DeliveryState::accepted)
        log_->mail("mail.accepted",box.value,sender,result.event,category,wire);
    return result;
}

// 在稳定的原事件身份下查找仍保留的事务。
InteractionRoutes::Exchange* InteractionRoutes::find(events::EventId event) {
    const auto found = std::ranges::find_if(exchanges_,[&](const auto& value) { return value.origin.id == event; });
    return found == exchanges_.end() ? nullptr : &*found;
}

// 从可信收件信封恢复原事件和发送实例，过期事项由收件器消费。
InteractionRoutes::Exchange* InteractionRoutes::original(const MailboxDelivery& delivery) {
    const auto body = Json::parse(delivery.event.payload->find("text")->as_string());
    auto* exchange = find({identity(body.at("eventDomain")),identity(body.at("event"))});
    return exchange && exchange->origin.owner == delivery.sender && exchange->main_box == delivery.mailbox ? exchange : nullptr;
}

// 为原插件登记独立的主 Agent 上下文地址。
// 步骤：读取登记建议 → 建立地址和唤醒规则 → 原子发布或关闭地址。
events::MailboxId InteractionRoutes::main_box(const plugins::RoutedEvent& origin) {
    if (!origin.mailbox.version) throw std::invalid_argument("interaction: plugin mailbox policy required");
    if (const auto known = main_boxes_.find(origin.owner); known != main_boxes_.end()) return known->second;
    const auto box = mail_.network->open(mail_.identity,origin.owner);
    std::vector<events::MailboxCategory> wake{{"interaction.ask.completed",1}};
    for (const auto& rule : origin.mailbox_categories)
        if (rule.flags & DFY_V2_MAIL_WAKE) wake.push_back({rule.category,rule.version});
    try {
        main_boxes_.emplace(origin.owner,box);
        intake_.watch(box,wake,
            [this](const MailboxDelivery& delivery) -> std::optional<std::string> {
                const auto wire = delivery.event.payload->find("text")->as_string();
                if (Json::parse(wire).at("requiresAsk") == true && !original(delivery)) return {};
                return std::string(wire);
            },origin.mailbox.flags & DFY_V2_MAIL_HIGH ? MailboxPriority::high : MailboxPriority::normal,
            [this](const MailboxDelivery& delivery, agent::RunOptions& options) { configure_main(delivery,options); },
            [this] { return auto_wake_; });
    } catch (...) {
        main_boxes_.erase(origin.owner);
        mail_.network->close(mail_.identity,box);
        throw;
    }
    return box;
}

// 以当前信件绑定可用工具，模型只选择渠道或提交原答复。
// 步骤：核对事项和回合阶段 → 登记带身份闭包的工具 → 固定本轮快照。
void InteractionRoutes::configure_main(const MailboxDelivery& delivery, agent::RunOptions& options) {
    auto* exchange = original(delivery);
    if (!exchange) return;
    const auto category = delivery.event.payload->descriptor()->name();
    const bool reply = category == "interaction.ask.completed" || category == "interaction.ask.completed.passive";
    const auto id = exchange->origin.id;
    constexpr std::string_view ask_tool = "interaction/ask", reply_tool = "interaction/reply";
    const std::string tool(reply ? reply_tool : ask_tool);
    auto catalog = std::make_shared<agent::ToolCatalog>(1);
    catalog->add({tool,mail_.identity,1,reply ?
        model::ToolDefinition{"mail_reply","Return the original ASK result to the plugin that asked the question.",
            R"({"type":"object","properties":{},"additionalProperties":false})"} :
        model::ToolDefinition{"ask","Ask the original question. Choose the allowed game or terminal channel.",
            R"({"type":"object","properties":{"channel":{"type":"string","enum":["game","terminal"]}},"required":["channel"],"additionalProperties":false})"},true},
        // 只推进原收件事项，模型无法自填答复或改写回信地址。
        [this,id,reply](const agent::ToolInvocation& invocation) {
            auto* current = find(id);
            if (!current || !destination(current->origin).value) throw std::invalid_argument("interaction: original request unavailable");
            if (reply) {
                if (!current->result_queued) throw std::invalid_argument("interaction: answer has not arrived");
                current->reply_requested = true;
            } else open_ask(*current,Json::parse(invocation.call.arguments).at("channel"));
            return agent::completed_tool({true,agent::ToolEffect::applied,
                Json{{"ask",current->ask},{"status",reply ? "reply_accepted" : "awaiting_answer"}}.dump(),agent::ToolError::none});
        });
    options.tools = catalog->snapshot(std::span(&tool,1),true);
    options.system_instructions += reply ? " 本轮收到原问题的答复，调用 mail_reply 把原结果送回提问者。" :
        " 本轮收到子 Agent 求助。你可以用 ask 选择渠道向真人询问。工具返回等待编号后可以结束本轮。";
    main_run_tools_ = std::move(catalog);
}

// 单步接收事件并推进所有已接纳的原提问。
void InteractionRoutes::advance() {
    if (!intake_.busy()) main_run_tools_.reset();
    asks_.advance();
    if (auto event = plugins_.take_routed_event()) {
        const auto id = event->id;
        try { accept(std::move(*event)); }
        catch (const std::exception&) {
            if (log_) log_->append("event.rejected",{{"eventDomain",id.domain},{"event",id.sequence}});
        }
    }
    std::erase_if(exchanges_,[&](auto& exchange) { return advance_exchange(exchange); });
    std::erase_if(main_boxes_,[&](const auto& entry) {
        try { (void)mail_.network->sender(mail_.identity,entry.second); }
        catch (const std::invalid_argument&) { return true; }
        return false;
    });
}
}
