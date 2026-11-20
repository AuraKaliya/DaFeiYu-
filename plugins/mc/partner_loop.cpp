#include "partner_loop.hpp"
#include <array>

namespace dfy::mc {
namespace {
using Json = nlohmann::json;

// 借出仅本次 ABI 调用使用的字符串。
constexpr dfy_v2_string_view view(std::string_view bytes) noexcept { return {bytes.data(),bytes.size()}; }
constexpr std::array skills{view("mc.companion/observe"),view("mc.companion/move"),
    view("mc.companion/collect"),view("mc.companion/reply"),view("mc.companion/collect_confirmed")};
constexpr auto instructions = view("你是大肥鱼，用自己的角色和玩家一起玩。events 是按序收到的在场变化和玩家原话。"
    "结合当前约定、自己的打算和已确认的现场，自行选择行动、协商、拒绝或安静等待。"
    "要对玩家说话时使用回复工具；回合结束的文本是内部结算，不会发到游戏。"
    "如果这次采集的范围需要玩家明确批准，用 collect_confirmed，由本体选择询问渠道。");
constexpr auto review_instructions = view("你是正在游戏里行动的大肥鱼。根据分工、现场和新事件决定是否提前停下列出的原动作。"
    "只回答 stop 或 continue。回答 stop 才会请求中止。这个判断不向玩家发言，已经发生的动作效果继续保留。");
}

// 由玩家入口显式启用，插件加载本身不读取游戏或提交模型。
// 步骤：协商子会话及通信接口 → 建立定向地址 → 开启配对玩家事件循环。
uint32_t PartnerLoop::listen(dfy_v2_query_interface_fn host, const BridgeConfig& config) {
    if (phase_ == DFY_V2_PARTNER_LISTENING) return DFY_V2_OK;
    if (phase_ == DFY_V2_PARTNER_STOPPING) return DFY_V2_BUSY;
    if (config.key.empty()) return DFY_V2_UNSUPPORTED;
    const void* api = nullptr;
    const auto result = host(view(DFY_V2_AGENT_INTERFACE),DFY_V2_AGENT_VERSION,&api,nullptr);
    if (result || !api) return result ? result : DFY_V2_UNSUPPORTED;
    const auto& agent = *static_cast<const dfy_v2_agent_api*>(api);
    if (agent.header.struct_size != sizeof(agent) || agent.header.interface_version != DFY_V2_AGENT_VERSION ||
        !agent.open || !agent.submit || !agent.status || !agent.cancel || !agent.close) return DFY_V2_ABI_MISMATCH;
    api = nullptr;
    if (const auto status = host(view(DFY_V2_MAILBOX_INTERFACE),DFY_V2_MAILBOX_VERSION,&api,nullptr); status || !api)
        return status ? status : DFY_V2_UNSUPPORTED;
    const auto& mail = *static_cast<const dfy_v2_mailbox_api*>(api);
    if (mail.header.struct_size != sizeof(mail) || mail.header.interface_version != DFY_V2_MAILBOX_VERSION ||
        !mail.attach || !mail.post || !mail.poll) return DFY_V2_ABI_MISMATCH;
    dfy_v2_mailbox_pair addresses{};
    if (const auto status = mail.attach(&addresses,nullptr)) return status;
    const void* text_api = nullptr;
    const void* reference_api = nullptr;
    if (host(view(DFY_V2_MAILBOX_TEXT_INTERFACE),DFY_V2_MAILBOX_TEXT_VERSION,&text_api,nullptr) ||
        host(view(DFY_V2_REFERENCE_INTERFACE),DFY_V2_REFERENCE_VERSION,&reference_api,nullptr) || !text_api || !reference_api)
        return DFY_V2_UNSUPPORTED;
    mail_text_ = static_cast<const dfy_v2_mailbox_text_api*>(text_api);
    refs_ = static_cast<const dfy_v2_reference_api*>(reference_api);
    if (mail_text_->header.struct_size != sizeof(*mail_text_) || refs_->header.struct_size != sizeof(*refs_) ||
        !mail_text_->inspect || !refs_->release) return DFY_V2_ABI_MISMATCH;
    agent_ = &agent;
    mail_ = &mail;
    phase_ = DFY_V2_PARTNER_LISTENING;
    return DFY_V2_OK;
}

// 保存协作取消责任，下一步停止读取并等待在途工作退出。
void PartnerLoop::stop() noexcept { if (phase_ == DFY_V2_PARTNER_LISTENING) phase_ = DFY_V2_PARTNER_STOPPING; }

// 在宿主已撤销全部子会话工作后释放插件本地读口。
void PartnerLoop::reset() noexcept {
    feed_.clear();
    phase_ = DFY_V2_PARTNER_CLOSED;
    agent_ = nullptr;
    mail_ = nullptr;
    mail_text_ = nullptr;
    refs_ = nullptr;
    confirmation_.reset();
    outbound_.clear();
    release_.reset();
    while (action_count_) actions_[--action_count_] = {};
    session_world_.clear();
    session_ = request_ = order_ = settled_ = 0;
    review_ = {};
    events_.clear();
    previous_.clear();
    reviewed_ = 0;
    stopped_work_ = false;
}

// 子回合业务结算及真正退出均由原会话报告。
// 步骤：核对请求 → 按需要提出取消 → 保留在途状态或记录实际退出水位。
bool PartnerLoop::settle(bool cancel) {
    if (!request_) return true;
    dfy_v2_agent_status state{};
    const auto result = agent_->status(session_,&state,nullptr);
    if (result == DFY_V2_INVALID_ARGUMENT && cancel) { request_ = 0; return true; }
    if (result || state.request != request_) throw std::runtime_error("MC partner: original turn unavailable");
    if (state.phase == DFY_V2_AGENT_QUEUED || state.phase == DFY_V2_AGENT_RUNNING) {
        if (cancel) agent_->cancel(session_,request_,nullptr);
        return false;
    }
    settled_ = order_;
    if (!permits(session_) && !cancel) previous_ = Json{{"events",events_},{"stopRequested",stopped_work_}};
    publish("mc.partner.turn",{{"generation",session_world_},{"order",order_},
        {"failed",state.phase == DFY_V2_AGENT_FAILED && permits(session_) && !cancel}});
    request_ = 0;
    return true;
}

// 原会话空闲后释放旧游戏上下文，下一世界重新签发身份。
void PartnerLoop::retire() {
    if (session_) agent_->close(session_,nullptr);
    session_world_.clear();
    session_ = order_ = settled_ = 0;
    previous_.clear();
}

// 将按序收到的原始事实及绑定游戏数据送入同一子会话。
// 步骤：创建或复用上下文 → 定格原玩家及候选范围 → 接纳后消费事实。
void PartnerLoop::submit() {
    if (!session_) {
        auto world = feed_.world();
        if (agent_->open(&session_,nullptr)) throw std::runtime_error("MC partner: cannot open child session");
        session_world_ = std::move(world);
    }
    auto facts = feed_.pending();
    const auto order = facts.back().at("sequence").get<std::uint64_t>();
    Json input{{"player",feed_.owner()},{"generation",session_world_},{"events",facts},{"lastObservation",feed_.observation()}};
    if (previous_.is_object() && !previous_.empty()) input["previousActivity"] = previous_;
    if (feed_.gap()) input["eventGap"] = true;
    const auto prompt = input.dump();
    const auto binding = Json{{"generation",session_world_},{"order",order}}.dump();
    const dfy_v2_agent_input run{view(prompt),instructions,view(binding),skills.data(),
        static_cast<uint32_t>(skills.size()),DFY_V2_AGENT_ALLOW_CHANGES};
    if (agent_->submit(session_,&run,&request_,nullptr)) throw std::runtime_error("MC partner: cannot queue child turn");
    order_ = order;
    reviewed_ = 0;
    stopped_work_ = false;
    events_ = std::move(facts);
    for (const auto& fact : events_) publish("mc.partner.event",{{"generation",session_world_},{"event",fact}});
    previous_.clear();
    feed_.consumed();
}

// 检查已观察到的新事实是否使旧回合失去继续接纳权限。
bool PartnerLoop::permits(std::uint64_t session) const noexcept {
    return session != session_ || (phase_ == DFY_V2_PARTNER_LISTENING && feed_.world() == session_world_ &&
        feed_.pending().empty() && !feed_.gap());
}

// 把原退出票据的效果与正文移动到本轮预留槽位。
void PartnerLoop::record(std::uint64_t session, std::uint64_t invocation, std::string_view skill, uint32_t effect, std::string output) noexcept {
    if (!session_ || session != session_ || !request_ || action_count_ >= actions_.size()) return;
    actions_[action_count_++] = {invocation,skill,effect,std::move(output)};
}

// 用固定分类封装已校验事实，消息正文大小保持在信箱上限内。
void PartnerLoop::publish(std::string_view category, const Json& payload) {
    if (outbound_.size() >= 128) throw std::logic_error("MC partner: outbox capacity exhausted");
    outbound_.push_back({category,payload.dump()});
}

// 交付实际退出的动作，每份回执保留原游戏、动作编号和效果类别。
// 步骤：按原票据顺序记录 → 提取实际游戏正文或注明省略 → 发布信箱事实并归还槽位。
void PartnerLoop::report_actions() {
    for (std::size_t index = 0; index < action_count_; ++index) {
        auto& action = actions_[index];
        Json fact{{"generation",session_world_},{"order",order_},{"invocation",action.id},{"skill",action.skill},{"effect",action.effect}};
        if (!action.body.empty() && action.body.size() <= 8*1024) fact["result"] = Json::parse(action.body);
        else if (!action.body.empty()) fact["bodyOmitted"] = true;
        publish("mc.partner.action",fact);
        action = {};
    }
    action_count_ = 0;
}

// ACK 只确认收进本体信箱，重投用同一发送序号。
// 步骤：检查队首及编号 → 投递 → 收到接纳回执后归还本地持有。
void PartnerLoop::flush() {
    if (outbound_.empty()) return;
    if (sent_ == UINT64_MAX) throw std::overflow_error("MC partner: outbox sequence exhausted");
    const auto& front = outbound_.front();
    dfy_v2_mailbox_receipt receipt{};
    const auto response = mail_->post(sent_+1,view(front.category),1,view(front.body),&receipt,nullptr);
    if (response == DFY_V2_BUSY || response == DFY_V2_RESOURCE_EXHAUSTED) return;
    if (response || (receipt.state != DFY_V2_MAILBOX_ACCEPTED && receipt.state != DFY_V2_MAILBOX_REPEATED))
        throw std::runtime_error("MC partner: invalid mailbox delivery");
    ++sent_;
    outbound_.pop_front();
}

// 原回合已退出后完成接管收尾，失联或超时仅能报告未获确认。
// 步骤：使用固定编号重读或提交 → 校验实际回执 → 发布释放或未核实结论。
bool PartnerLoop::release_game(const BridgeConfig& config, std::uint64_t instance) {
    using namespace std::chrono;
    auto& release = *release_;
    if (release.done) return true;
    const auto now = steady_clock::now();
    if (now >= release.deadline || release.world.empty()) {
        release.pipe.reset();
        publish("mc.partner.control",{{"generation",release.world},{"order",0},{"phase","unconfirmed"}});
        return release.done = true;
    }
    if (now < release.next) return false;
    try {
        if (release.id.empty()) {
            if (release_sequence_ == UINT64_MAX) throw std::overflow_error("MC partner: release sequence exhausted");
            release.id = "release."+std::to_string(instance)+"."+std::to_string(++release_sequence_);
            release.arguments = Json{{"generation",release.world}}.dump();
        }
        if (!release.pipe)
            release.pipe = std::make_unique<PipeExchange>(config.channel,control_request(release.id,config.key,release.arguments));
        const auto wire = release.pipe->poll();
        if (!wire) return false;
        release.pipe.reset();
        const auto result = control_reply(*wire,release.id,release.arguments);
        if (!result.empty()) {
            publish("mc.partner.control",Json::parse(result));
            return release.done = true;
        }
    } catch (const std::exception&) { release.pipe.reset(); }
    release.next = now+milliseconds(50);
    return false;
}

// 判断原生动作是否需要提前请求停止，保持业务处置与原动作退出分离。
// 步骤：检查原判断关联 → 忽略过期或非明确答复 → 在有效新事件上登记一次新判断。
void PartnerLoop::review(PartnerWork work, bool closing) {
    if (review_.request) {
        const bool current = !closing && request_ && review_.origin == request_ && review_.work == work.invocation &&
            review_.cursor == feed_.cursor();
        dfy_v2_agent_status status{};
        const auto result = agent_->status(review_.session,&status,nullptr);
        if (!result && (status.phase == DFY_V2_AGENT_QUEUED || status.phase == DFY_V2_AGENT_RUNNING)) {
            if (!current) agent_->cancel(review_.session,review_.request,nullptr);
            return;
        }
        if (!result && current && status.request == review_.request && status.phase == DFY_V2_AGENT_SUCCEEDED &&
            std::string_view(status.answer.data,status.answer.length) == "stop") {
            stopped_work_ = agent_->cancel(session_,request_,nullptr) == DFY_V2_OK;
        }
        agent_->close(review_.session,nullptr);
        review_ = {};
    }
    if (closing || !request_ || !work.invocation || stopped_work_ || feed_.pending().empty() || feed_.cursor() <= reviewed_) return;
    const auto input = Json{{"currentEvents",events_},{"newEvents",feed_.pending()},
        {"work",{{"skill",work.skill},{"candidate",work.invocation},{"state","running"}}},
        {"player",feed_.owner()},{"generation",session_world_},{"lastObservation",feed_.observation()}}.dump();
    reviewed_ = feed_.cursor();
    const dfy_v2_agent_input assessment{view(input),review_instructions,{},{},0,0};
    if (agent_->open(&review_.session,nullptr)) return;
    if (agent_->submit(review_.session,&assessment,&review_.request,nullptr)) {
        agent_->close(review_.session,nullptr);
        review_ = {};
        return;
    }
    review_.origin = request_;
    review_.work = work.invocation;
    review_.cursor = reviewed_;
}

// 驱动插件自己的事件与行动循环，游戏解绑优先结束原请求。
// 步骤：推进原生读取 → 核对在途回合 → 释放旧上下文或接纳最新事件。
void PartnerLoop::advance(const BridgeConfig& config, std::uint64_t instance, PartnerWork work) {
    if (phase_ == DFY_V2_PARTNER_CLOSED) return;
    report_actions();
    flush();
    if (phase_ == DFY_V2_PARTNER_LISTENING) {
        feed_.advance(config,instance);
        if (!feed_.control().is_null()) stop();
        if (phase_ == DFY_V2_PARTNER_LISTENING) {
            advance_confirmation();
            intake_confirmation();
        }
    }
    const bool closing = phase_ == DFY_V2_PARTNER_STOPPING;
    if (closing && !release_) {
        release_.emplace();
        release_->world = feed_.world().empty() ? session_world_ : feed_.world();
        release_->deadline = std::chrono::steady_clock::now()+std::chrono::seconds(5);
        feed_.clear();
    }
    const bool replaced = session_ && feed_.world() != session_world_;
    review(work,closing || replaced);
    if (!settle(closing || replaced)) return;
    if (closing) {
        if (review_.request || !release_game(config,instance) || !outbound_.empty()) return;
        retire(); reset(); return;
    }
    if (replaced) retire();
    if (feed_.ready() && !feed_.pending().empty() && outbound_.size() <= 54) submit();
}

// 借出最新角色状态，世界字符串只在本次 ABI 调用内有效。
dfy_v2_partner_status PartnerLoop::status() const noexcept {
    return {phase_,feed_.ready(),session_,feed_.cursor(),feed_.departed(),settled_,view(feed_.world()),view(feed_.owner())};
}
}
