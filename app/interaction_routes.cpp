#include "interaction_routes.hpp"
#include "run_log.hpp"
#include "transition_notice.hpp"
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
    : plugins_(plugins), mail_(std::move(mail)), intake_(intake), clock_(std::move(clock)), log_(std::move(log)),
      asks_(clock_,[this](auto ask, const auto& request) { observe_ask(ask,request); }),
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

// 将类型化路由应用到原事项或渠道。
// 步骤：核对原地址 → 处理渠道协议 → 接纳有界的新事项。
bool InteractionRoutes::dispatch(plugins::RoutedEvent event, EventRoute route, bool denied) {
    if (!destination(event).value) return false;
    if (const auto* receipt = std::get_if<AskReceipt>(&route)) {
        auto* exchange = find(receipt->origin);
        if (!exchange || !exchange->delivery) return false;
        const bool first = !exchange->delivery->acknowledged();
        if (!exchange->delivery->acknowledge(event.owner,*receipt,log_.get())) return false;
        if (first) notify_transition(plugins_,log_.get(),{DFY_V2_TRANSITION_REPLY_ACKNOWLEDGED,event.owner,
            receipt->origin,receipt->disposition,0,receipt->ask,exchange->delivery->message()});
        return true;
    }
    if (const auto* feedback = std::get_if<AskFeedback>(&route)) {
        const bool presented = feedback->action == AskFeedback::Action::presented;
        bool accepted = false;
        if (feedback->channel == "game") {
            if (presented) accepted = asks_.presented(feedback->ask,event.owner,feedback->channel,feedback->delivered);
            else accepted = asks_.answer(feedback->ask,event.owner,feedback->channel,feedback->recipient,feedback->answer);
        }
        if (log_) log_->append(presented ? "ask.presented" : "ask.answer",{{"ask",feedback->ask},{"instance",event.owner},
            {"accepted",accepted},{"body",Json::parse(event.projection)}});
        return accepted;
    }
    if (exchanges_.size() == 16) throw std::length_error("interaction: pending route capacity exhausted");
    if (std::holds_alternative<MailRoute>(route)) {
        if (denied) return false;
        const auto box = main_box(event);
        exchanges_.push_back({std::move(event),0,0,0,clock_()+std::chrono::seconds(120),box});
        return true;
    }
    auto& question = std::get<AskRoute>(route);
    if (std::ranges::any_of(exchanges_,[&](const auto& other) {
        return other.origin.owner == event.owner && other.item == question.item && other.revision == question.revision;
    })) return false;
    const auto box = question.via_main && !denied ? main_box(event) : events::MailboxId{};
    exchanges_.push_back({std::move(event),question.item,question.revision,0,clock_()+std::chrono::milliseconds(question.wait_ms),box});
    auto& exchange = exchanges_.back();
    exchange.question.emplace(std::move(question));
    if (denied) exchange.failure = "route_denied";
    else if (exchange.question->channel == "terminal" && !terminal_available_) exchange.failure = "channel_unavailable";
    if (!exchange.question->via_main && exchange.failure.empty()) {
        try { open_ask(exchange,exchange.question->channel); }
        catch (...) { exchanges_.pop_back(); throw; }
    }
    return !denied;
}

// 关联票据状态和原事件，观察接口只暴露阶段与渠道。
// 步骤：找到原事项 → 选择阶段通知 → 记录无票据正文的状态迁移。
void InteractionRoutes::observe_ask(std::uint64_t ask, const AskRequest& request) {
    const auto found = std::ranges::find(exchanges_,ask,&Exchange::ask);
    if (found == exchanges_.end()) return;
    const auto point = request.phase == AskPhase::waiting ? DFY_V2_TRANSITION_ASK_PRESENTED :
        request.phase == AskPhase::answered ? DFY_V2_TRANSITION_ASK_ANSWERED : DFY_V2_TRANSITION_ASK_ENDED;
    if (log_) log_->append("ask.transition",{{"ask",ask},{"eventDomain",found->origin.id.domain},{"event",found->origin.id.sequence},
        {"phase",ask_phase(request.phase)},{"channel",request.spec.channel}});
    notify_transition(plugins_,log_.get(),{point,request.owner,found->origin.id,request.spec.channel,
        static_cast<std::uint32_t>(request.phase),ask});
}

// 只在首次信箱接纳时广播终态入箱。
void InteractionRoutes::advance_delivery(Exchange& exchange) {
    auto& delivery = *exchange.delivery;
    if (delivery.advance(*mail_.network,mail_.identity,log_.get()))
        notify_transition(plugins_,log_.get(),{DFY_V2_TRANSITION_REPLY_ENQUEUED,exchange.origin.owner,exchange.origin.id,
            delivery.category(),0,exchange.ask,delivery.message()});
}

// 对原事件的游戏声明和双向终端能力取当前快照。
std::vector<std::string_view> InteractionRoutes::available_channels(std::string_view origin_channel) const {
    std::vector<std::string_view> channels;
    if (origin_channel == "game") channels.push_back("game");
    if (terminal_available_) channels.push_back("terminal");
    return channels;
}

// 创建并记录原事项的唯一提问，重试复用同一票据。
void InteractionRoutes::open_ask(Exchange& exchange, std::string channel) {
    if (!exchange.failure.empty() || exchange.delivery) throw std::invalid_argument("interaction: ASK route has ended");
    const auto& question = exchange.question.value();
    const auto channels = available_channels(question.channel);
    if (std::ranges::find(channels,channel) == channels.end()) {
        if (log_) log_->append("ask.channel_unavailable",{{"eventDomain",exchange.origin.id.domain},{"event",exchange.origin.id.sequence},
            {"item",exchange.item},{"channel",channel},{"availableChannels",channels}});
        throw std::invalid_argument("interaction: ASK channel unavailable");
    }
    if (exchange.ask) {
        if (asks_.status(exchange.ask)->spec.channel != channel) throw std::invalid_argument("interaction: existing ASK uses a different channel");
        return;
    }
    exchange.ask = asks_.open(exchange.origin.owner,{question.question,question.recipient,std::move(channel),
        question.choices,exchange.answer_deadline});
    if (log_) log_->append("ask.opened",{{"ask",exchange.ask},{"eventDomain",exchange.origin.id.domain},
        {"event",exchange.origin.id.sequence},{"item",exchange.item},{"revision",exchange.revision},
        {"path",exchange.main_box.value ? "main" : "direct"},{"question",question.question},{"channel",asks_.status(exchange.ask)->spec.channel},
        {"answerWaitMs",std::chrono::duration_cast<std::chrono::milliseconds>(exchange.answer_deadline-clock_()).count()}});
}

// 推进原问题和终态投递，票据由原插件的收件回执结算。
// 步骤：校验收件回执及原实例 → 推进首次投递或呈现 → 排队终态并等待确认。
bool InteractionRoutes::advance_exchange(Exchange& exchange) {
    const auto inbox = destination(exchange.origin);
    const bool acknowledged = exchange.delivery && exchange.delivery->acknowledged();
    if (acknowledged || !inbox.value) {
        asks_.cancel(exchange.ask,exchange.origin.owner);
        asks_.release(exchange.ask);
        if (!inbox.value && !acknowledged && exchange.item && log_) log_->append("interaction.abandoned",
            {{"ask",exchange.ask},{"eventDomain",exchange.origin.id.domain},{"event",exchange.origin.id.sequence},
             {"item",exchange.item},{"reason","original_instance_unavailable"}});
        return !inbox.value || !exchange.review_run.session || !intake_.busy();
    }
    if (exchange.delivery) {
        advance_delivery(exchange);
        return false;
    }
    if (exchange.question && !exchange.ask && exchange.failure.empty() && !terminal_available_ && exchange.question->channel == "terminal")
        exchange.failure = "channel_unavailable";
    if (!exchange.ask && exchange.failure.empty() && clock_() < exchange.answer_deadline) {
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
    if (!exchange.item) return true;
    const auto* request = asks_.status(exchange.ask);
    if (request && request->spec.channel == "terminal" && !terminal_available_) asks_.cancel(exchange.ask,exchange.origin.owner);
    const bool pending = request && (request->phase == AskPhase::presenting || request->phase == AskPhase::waiting);
    if (pending && (exchange.sent || request->spec.channel == "terminal")) return false;
    const bool to_main = request && !pending && exchange.main_box.value && exchange.reply != ReplyState::requested;
    if (to_main && exchange.reply != ReplyState::pending && exchange.reply != ReplyState::retry) return false;
    const bool wake = exchange.reply != ReplyState::retry && (exchange.origin.mailbox.flags & DFY_V2_MAIL_WAKE);
    const char* completion = wake ? "interaction.ask.completed" : "interaction.ask.completed.passive";
    const char* category = pending ? "interaction.ask.present" : to_main ? completion :
        exchange.main_box.value ? "interaction.mail.reply" : "interaction.ask.result";
    Json message{{"ask",exchange.ask},{"item",exchange.item},{"revision",exchange.revision},{"recipient",exchange.question->recipient},
        {"channel",request ? request->spec.channel : std::string{}},{"requiresAsk",true}};
    if (pending) { message["question"] = request->spec.question; message["choices"] = request->spec.choices; }
    else {
        message["status"] = request ? ask_phase(request->phase) : exchange.failure.empty() ? "expired" : "failed";
        message["answer"] = request ? request->answer : "";
        if (!exchange.failure.empty()) message["reason"] = exchange.failure;
    }
    message["eventDomain"] = exchange.origin.id.domain;
    message["event"] = exchange.origin.id.sequence;
    if (!pending && !to_main) {
        exchange.delivery.emplace(exchange.origin.owner,AskReceipt{exchange.origin.id,exchange.ask,exchange.item,exchange.revision,{}},
            inbox,category,message.dump());
        advance_delivery(exchange);
        return false;
    }
    try {
        const auto box = to_main ? exchange.main_box : inbox;
        const auto receipt = post(to_main ? exchange.origin.owner : mail_.identity,box,category,1,message.dump());
        if (to_main) exchange.main_result = receipt.event;
        if (log_) log_->append("interaction.delivered",{{"ask",exchange.ask},{"eventDomain",exchange.origin.id.domain},
            {"event",exchange.origin.id.sequence},{"item",exchange.item},{"category",category},{"mailbox",box.value},
            {"message",receipt.event.sequence},{"body",message}});
    } catch (const std::length_error&) { return false; }
    if (to_main) exchange.reply = ReplyState::queued;
    else exchange.sent = true;
    return false;
}

// 查询应由终端下一次输出的原问题。
std::optional<std::pair<std::uint64_t,AskSpec>> InteractionRoutes::console_question() const {
    if (!terminal_available_) return {};
    for (const auto& exchange : exchanges_) {
        const auto* request = asks_.status(exchange.ask);
        if (request && request->phase == AskPhase::presenting && request->spec.channel == "terminal")
            return std::pair{exchange.ask,request->spec};
    }
    return {};
}

// 从本体终端按原身份确认呈现或接纳一条答复。
bool InteractionRoutes::console_answer(std::uint64_t id, std::optional<std::string> answer) {
    if (!terminal_available_) return false;
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
    if (!exchange || exchange->delivery || !exchange->failure.empty() || exchange->origin.owner != delivery.sender || exchange->main_box != delivery.mailbox)
        return nullptr;
    return exchange;
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
            [this](const MailboxDelivery& delivery) -> std::optional<MailboxProjection> {
                const auto wire = delivery.event.payload->find("text")->as_string();
                const bool bound = Json::parse(wire).at("requiresAsk") == true;
                if (bound && !original(delivery)) return {};
                return MailboxProjection{std::string(wire),bound};
            },origin.mailbox.flags & DFY_V2_MAIL_HIGH ? MailboxPriority::high : MailboxPriority::normal,
            [this](const MailboxDelivery& delivery, agent::RunOptions& options) { configure_main(delivery,options); },
            [this] { return auto_wake_; },[this](const MailboxDelivery& delivery, agent::RunId run) { main_accepted(delivery,run); });
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
    const auto id = exchange->origin.id;
    if (category == "interaction.ask.completed" || category == "interaction.ask.completed.passive") {
        if (exchange->reply != ReplyState::queued || exchange->main_result != delivery.event.id) return;
        main_run_tools_ = bind_mail_reply(mail_.identity,{id,delivery.event.id,exchange->origin.owner,exchange->ask,exchange->item,exchange->revision},
            [this](const auto& binding, const auto& invocation) { request_reply(binding,invocation); },options);
        return;
    }
    const std::string tool = "interaction/ask";
    const auto channels = available_channels(exchange->question->channel);
    if (channels.empty()) return;
    auto schema = Json::parse(R"({"type":"object","properties":{"channel":{"type":"string"}},"required":["channel"],"additionalProperties":false})");
    schema["properties"]["channel"]["enum"] = channels;
    auto catalog = std::make_shared<agent::ToolCatalog>(1);
    catalog->add({tool,mail_.identity,1,{"ask","Ask the original question using an available channel.",schema.dump()},true},
        // 在原事件身份下核对并开启提问。
        [this,id](const agent::ToolInvocation& invocation) {
            auto* current = find(id);
            if (!current || !destination(current->origin).value) throw std::invalid_argument("interaction: original request unavailable");
            open_ask(*current,Json::parse(invocation.call.arguments).at("channel"));
            return agent::completed_tool({true,agent::ToolEffect::applied,
                Json{{"ask",current->ask},{"status","awaiting_answer"}}.dump(),agent::ToolError::none});
        });
    options.tools = catalog->snapshot(std::span(&tool,1),true);
    options.immutable_tool_arguments = "ask";
    options.system_instructions += " 本轮收到子 Agent 求助。你可以用 ask 选择渠道向真人询问。工具返回等待编号后可以结束本轮。";
    main_run_tools_ = std::move(catalog);
}

// 固定实际接纳原结果的主回合及其信箱序号。
void InteractionRoutes::main_accepted(const MailboxDelivery& delivery, agent::RunId run) {
    for (auto& exchange : exchanges_) if (exchange.reply == ReplyState::queued && exchange.main_result == delivery.event.id &&
        exchange.main_box == delivery.mailbox && exchange.origin.owner == delivery.sender) {
        exchange.review_run = run;
        exchange.reply = ReplyState::reviewing;
        return;
    }
}

// 接纳回合绑定的原回信意图，同一绑定的重复调用复用其结果。
// 步骤：核对原消息及运行 → 识别重投 → 固定待投递意图并记录调用。
void InteractionRoutes::request_reply(const ReplyBinding& binding, const agent::ToolInvocation& invocation) {
    auto* exchange = find(binding.origin);
    const auto run = invocation.attempt.step.run;
    if (!exchange || !destination(exchange->origin).value || exchange->origin.owner != binding.owner ||
        exchange->main_result != binding.source_message || exchange->ask != binding.ask || exchange->item != binding.item ||
        exchange->revision != binding.revision || exchange->review_run != run ||
        (exchange->reply != ReplyState::reviewing && exchange->reply != ReplyState::requested))
        throw std::invalid_argument("interaction: reply binding unavailable");
    if (exchange->reply == ReplyState::requested) return;
    exchange->reply = ReplyState::requested;
    if (log_) log_->append("reply.intent",{{"ask",binding.ask},{"item",binding.item},{"revision",binding.revision},
        {"eventDomain",binding.origin.domain},{"event",binding.origin.sequence},{"session",run.session},{"run",run.sequence},
        {"message",binding.source_message.sequence},{"callId",invocation.call.id}});
}

// 主回合实际退出后，缺少转发意图的原答复转为待复审。
void InteractionRoutes::settle_review() {
    if (intake_.busy()) return;
    main_run_tools_.reset();
    for (auto& exchange : exchanges_) if (exchange.reply == ReplyState::reviewing) {
        exchange.reply = ReplyState::deferred;
        if (log_) log_->append("reply.review_required",{{"ask",exchange.ask},{"eventDomain",exchange.origin.id.domain},
            {"event",exchange.origin.id.sequence},{"session",exchange.review_run.session},{"run",exchange.review_run.sequence}});
    }
}

// 仅在显式审查时将保留的答复放回原信箱。
// 步骤：结算已退出的回合 → 恢复待复审结果 → 通过原邮箱规则参与本次取件。
void InteractionRoutes::prepare_review() {
    if (intake_.busy()) return;
    settle_review();
    for (auto& exchange : exchanges_) if (exchange.reply == ReplyState::deferred) exchange.reply = ReplyState::retry;
    advance();
}

// 推进已接纳的原提问并清理已撤销的信箱。
void InteractionRoutes::advance() {
    settle_review();
    asks_.advance();
    std::erase_if(exchanges_,[&](auto& exchange) { return advance_exchange(exchange); });
    std::erase_if(main_boxes_,[&](const auto& entry) {
        try { (void)mail_.network->sender(mail_.identity,entry.second); }
        catch (const std::invalid_argument&) { return true; }
        return false;
    });
}
}
