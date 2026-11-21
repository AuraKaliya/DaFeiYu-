#include "partner_loop.hpp"
#include <array>

namespace dfy::mc {
namespace {
using Json = nlohmann::json;

// 借出仅本次 ABI 调用使用的字符串。
constexpr dfy_v2_string_view view(std::string_view bytes) noexcept { return {bytes.data(),bytes.size()}; }
constexpr std::array delegated_skills{view("mc.companion/observe"),view("mc.companion/reply"),view("mc.companion/collect")};
constexpr auto instructions = view("你是大肥鱼，用自己的角色和玩家一起玩。events 是按序收到的在场变化和玩家原话。"
    "结合当前约定、自己的打算和已确认的现场，自行选择行动、协商、拒绝或安静等待。"
    "要对玩家说话时使用回复工具；回合结束的文本是内部结算，不会发到游戏。"
    "有持续分工时用 task_update 保存共同目标、已知事实和待核实条件。继续、等待现场变化、等待玩家或结束由你选择。"
    "某回合因 task.continue 或 scene.changed 唤醒时可能没有新聊天，结合任务和回执继续判断。首次入场无需固定问候。"
    "工具未展开时用 skills 查看分类；输入 categories 选择需要的分类，具体工具及参数会在下一回合提供。"
    "对需要明确批准的动作，在该动作的 approval 参数选择 game 或 main，分别在游戏询问或交给本体选择渠道。");
constexpr auto review_instructions = view("你是正在游戏里行动的大肥鱼。根据分工、现场和新事件决定是否提前停下列出的原动作。"
    "只回答 stop 或 continue。回答 stop 才会请求中止。这个判断不向玩家发言，已经发生的动作效果继续保留。");
constexpr auto delegation_instructions = view("delegation 来自大肥鱼本体，保留原编号。核对现场后回答委托。"
    "委托中的采集须征求当前玩家批准，默认由本体选择渠道。回复工具用于实际向玩家说话，最后的文本是给本体的报告。");

// 将基本观察、沟通和发现能力作为同场陪玩的初始契约。
bool base_category(SkillCategory category) noexcept {
    return category == SkillCategory::control || category == SkillCategory::query || category == SkillCategory::observation ||
        category == SkillCategory::communication;
}
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
    task_ = {};
    expanded_.reset();
    expansion_pending_ = false;
    phase_ = DFY_V2_PARTNER_CLOSED;
    agent_ = nullptr;
    mail_ = nullptr;
    mail_text_ = nullptr;
    refs_ = nullptr;
    confirmation_.reset();
    presentation_.reset();
    awaiting_delegation_.reset();
    active_delegation_.reset();
    last_task_ = 0;
    outbound_.clear();
    release_.reset();
    while (action_count_) actions_[--action_count_] = {};
    session_world_.clear();
    session_ = request_ = order_ = settled_ = 0;
    review_ = {};
    events_.clear();
    recent_events_.clear();
    previous_.clear();
    reviewed_ = 0;
    stopped_work_ = false;
    question_.reset(); question_work_.reset(); question_result_.clear(); question_wake_ = false; question_outbound_.clear();
}

// 子回合业务结算及真正退出均由原会话报告。
// 步骤：核对请求 → 按需要提出取消 → 保留在途状态或记录实际退出水位。
bool PartnerLoop::settle(bool cancel) {
    if (!request_) return true;
    dfy_v2_agent_status state{};
    const auto result = agent_->status(session_,&state,nullptr);
    if (result == DFY_V2_INVALID_ARGUMENT && cancel) state.phase = DFY_V2_AGENT_CANCELLED;
    else if (result || state.request != request_) throw std::runtime_error("MC partner: original turn unavailable");
    if (state.phase == DFY_V2_AGENT_QUEUED || state.phase == DFY_V2_AGENT_RUNNING) {
        if (cancel) agent_->cancel(session_,request_,nullptr);
        return false;
    }
    if (!active_delegation_ && (cancel || state.phase != DFY_V2_AGENT_SUCCEEDED)) {
        task_.disarm();
        expansion_pending_ = false;
    }
    if (cancel || autonomous_wake().empty()) settled_ = order_;
    if (!permits(session_) && !cancel) previous_ = Json{{"events",events_},{"stopRequested",stopped_work_}};
    if (active_delegation_) {
        const bool answered = !cancel && state.phase == DFY_V2_AGENT_SUCCEEDED && state.answer.length && state.answer.length <= 2048;
        const auto text = answered ? std::string(state.answer.data,state.answer.length) : std::string("伙伴未交付本次委托的文字答复，请查实际回执");
        publish("mc.partner.report",{{"generation",active_delegation_->world},{"id",active_delegation_->id},{"ok",answered},{"text",text}});
        active_delegation_.reset();
    } else publish("mc.partner.turn",{{"generation",session_world_},{"order",order_},{"session",session_},{"request",request_},
        {"failed",state.phase == DFY_V2_AGENT_FAILED && permits(session_) && !cancel}});
    request_ = 0;
    return true;
}

// 原会话空闲后释放旧游戏上下文，下一世界重新签发身份。
void PartnerLoop::retire() {
    if (session_) agent_->close(session_,nullptr);
    session_world_.clear();
    session_ = order_ = settled_ = 0;
    task_ = {};
    expanded_.reset();
    expansion_pending_ = false;
    recent_events_.clear();
    previous_.clear();
    question_.reset(); question_result_.clear(); question_wake_ = false;
}

// 将按序收到的原始事实及绑定游戏数据送入同一子会话。
// 步骤：创建或复用上下文 → 定格原玩家及候选范围 → 接纳后消费事实。
void PartnerLoop::submit(std::string_view wake) {
    if (!session_) {
        auto world = feed_.world();
        if (agent_->open(&session_,nullptr)) throw std::runtime_error("MC partner: cannot open child session");
        session_world_ = std::move(world);
    }
    auto facts = feed_.pending();
    const bool delegated = wake == "main.delegation";
    const bool answer = wake == "question.result";
    const bool autonomous = facts.empty() && !delegated && !answer;
    const auto order = facts.empty() ? feed_.cursor() : facts.back().at("sequence").get<std::uint64_t>();
    Json input{{"player",feed_.owner()},{"generation",session_world_},{"events",facts},{"lastObservation",feed_.observation()},
        {"task",task_.context()},{"trigger",wake}};
    if (feed_.scene_revision()) input["sceneRevision"] = feed_.scene_revision();
    if (autonomous) input["recentEvents"] = recent_events_;
    if (!question_result_.is_null() && !question_result_.empty()) input["questionResult"] = question_result_;
    if (question_) input["pendingQuestion"] = {{"item",question_->item},{"question",question_->text},{"escalated",question_->escalated}};
    if (delegated) input["delegation"] = {{"id",awaiting_delegation_->id},{"text",awaiting_delegation_->text}};
    if (previous_.is_object() && !previous_.empty()) input["previousActivity"] = previous_;
    if (feed_.gap()) input["eventGap"] = true;
    const auto prompt = input.dump();
    const auto binding = Json{{"generation",session_world_},{"order",order}}.dump();
    std::array<std::string,partner_skills.size()> ids;
    std::array<dfy_v2_string_view,partner_skills.size()> visible{};
    std::size_t count = 0;
    if (!delegated) for (const auto& skill : partner_skills) {
        if (skill.kind == SkillKind::confirmed || (skill.kind == SkillKind::question && !question_events_)) continue;
        const auto category = skill.category;
        if (!base_category(category) && !expanded_.test(static_cast<std::size_t>(category))) continue;
        ids[count] = "mc.companion/"+std::string(skill.name);
        visible[count] = view(ids[count]);
        ++count;
    }
    const dfy_v2_agent_input run{view(prompt),delegated ? delegation_instructions : instructions,view(binding),
        delegated ? delegated_skills.data() : visible.data(),
        static_cast<uint32_t>(delegated ? delegated_skills.size() : count),DFY_V2_AGENT_ALLOW_CHANGES};
    if (agent_->submit(session_,&run,&request_,nullptr)) throw std::runtime_error("MC partner: cannot queue child turn");
    if (!delegated) {
        task_.submitted(!facts.empty() || answer,autonomous);
        question_wake_ = false;
        expansion_pending_ = false;
    }
    order_ = order;
    reviewed_ = 0;
    stopped_work_ = false;
    events_ = std::move(facts);
    if (!events_.empty()) recent_events_ = events_;
    if (delegated) { active_delegation_ = std::move(awaiting_delegation_); awaiting_delegation_.reset(); }
    for (const auto& fact : events_) publish("mc.partner.event",{{"generation",session_world_},{"event",fact}});
    previous_.clear();
    feed_.consumed();
}

// 检查已观察到的新事实是否使旧回合失去继续接纳权限。
bool PartnerLoop::permits(std::uint64_t session) const noexcept {
    return session != session_ || (phase_ == DFY_V2_PARTNER_LISTENING && feed_.world() == session_world_ &&
        feed_.pending().empty() && !feed_.gap());
}

// 只有处理当前世界的原子会话能修改局内状态，旧事件不能更新任务或展开范围。
Json PartnerLoop::local_skill(std::uint64_t session, std::uint64_t invocation, std::string_view skill, std::string_view envelope) {
    if (session != session_ || !request_ || active_delegation_ || !permits(session) || feed_.departed() || !feed_.ready())
        throw std::invalid_argument("MC partner: task owner unavailable");
    const auto input = Json::parse(envelope);
    const auto& source = input.at("context");
    if (source.at("generation") != session_world_ || source.at("order") != order_)
        throw std::invalid_argument("MC partner: stale task context");
    const auto& args = input.at("arguments");
    if (skill == "ask_player") return open_question(invocation,args);
    return skill == "skills" ? discover(args) : task_.update(args,feed_.observation(),feed_.scene_revision());
}

// 输出游戏分类和已实现的工具，展开请求只影响后续回合的固定快照。
// 步骤：核对选择 → 汇总实现状态 → 在选择实际变化后登记一次唤醒。
Json PartnerLoop::discover(const Json& arguments) {
    const auto wanted = arguments.value("categories",std::vector<std::string>{});
    if (wanted.size() > 3) throw std::length_error("MC skills: choose up to three categories");
    std::bitset<categories.size()> next;
    for (const auto& name : wanted) {
        const auto found = std::find(categories.begin(),categories.end(),name);
        if (found == categories.end()) throw std::invalid_argument("MC skills: unknown category");
        next.set(found-categories.begin());
    }
    Json directory = Json::array();
    for (std::size_t index = 0; index < categories.size(); ++index) {
        Json names = Json::array();
        for (const auto& skill : partner_skills) if (skill.kind != SkillKind::confirmed && static_cast<std::size_t>(skill.category) == index)
            names.push_back(skill.name);
        if (names.empty() || base_category(static_cast<SkillCategory>(index))) next.reset(index);
        directory.push_back({{"category",categories[index]},{"available",!names.empty()},{"tools",std::move(names)}});
    }
    const bool changed = next.any() && next != expanded_;
    if (changed) {
        expanded_ = next;
        expansion_pending_ = true;
    }
    return {{"status",changed ? "scheduled" : "listed"},{"categories",std::move(directory)},
        {"wake",expansion_pending_ && task_.can_continue()}};
}

// 新输入优先，断线、缺口和自主预算均可阻止调度下一决策。
std::string_view PartnerLoop::autonomous_wake() const {
    if (!feed_.ready() || !feed_.pending().empty() || feed_.gap() || feed_.departed()) return {};
    if (expansion_pending_ && task_.can_continue()) return "skills.expanded";
    return task_.trigger(feed_.observation(),feed_.scene_revision());
}

// 把原退出票据的效果与正文移动到本轮预留槽位。
void PartnerLoop::record(std::uint64_t session, std::uint64_t invocation, std::string_view skill, uint32_t effect,
    std::string work, std::string output) noexcept {
    if (!session_ || session != session_ || !request_ || action_count_ >= actions_.size()) return;
    actions_[action_count_++] = {invocation,skill,effect,std::move(work),std::move(output)};
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
        Json fact{{"generation",session_world_},{"order",order_},{"session",session_},{"request",request_},
            {"invocation",action.id},{"skill",action.skill},{"effect",action.effect},{"bridgeWork",action.work}};
        if (active_delegation_) fact["task"] = active_delegation_->id;
        if (!action.body.empty() && action.body.size() <= 8*1024) fact["result"] = Json::parse(action.body);
        else if (!action.body.empty()) fact["bodyOmitted"] = true;
        if (!active_delegation_) task_.record(fact);
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
    // 与当前在途动作完全对应的唯一批准仍留在收件记录，已由确认状态机处理。
    if (confirmation_ && confirmation_->request == work.invocation && confirmation_->approved_order == feed_.cursor() &&
        feed_.pending().size() == 1 && confirmation_->approved_order > confirmation_->order) return;
    if (question_ && question_->approval && question_->item == work.invocation && question_->answer_order == feed_.cursor() &&
        feed_.pending().size() == 1) return;
    const auto input = Json{{"currentEvents",events_.empty() ? recent_events_ : events_},{"newEvents",feed_.pending()},
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
            intake(work);
        }
    }
    const bool closing = phase_ == DFY_V2_PARTNER_STOPPING;
    if (closing && !release_) {
        release_.emplace();
        release_->world = feed_.world().empty() ? session_world_ : feed_.world();
        release_->deadline = std::chrono::steady_clock::now()+std::chrono::seconds(5);
        feed_.clear();
    }
    advance_confirmation_presentation(config,instance,closing);
    advance_question(config,instance,closing);
    if (awaiting_delegation_ && (closing || awaiting_delegation_->world != feed_.world() || feed_.departed())) {
        publish("mc.partner.report",{{"generation",awaiting_delegation_->world},{"id",awaiting_delegation_->id},{"ok",false},
            {"text","原游戏世界或配对玩家已离开"}});
        awaiting_delegation_.reset();
    }
    const bool replaced = session_ && feed_.world() != session_world_;
    review(work,closing || replaced);
    if (!settle(closing || replaced)) return;
    if (closing) {
        if (review_.request || presentation_ || question_work_ || !release_game(config,instance) || !outbound_.empty()) return;
        retire(); reset(); return;
    }
    if (replaced) retire();
    if (feed_.departed()) {
        task_ = {};
        expanded_.reset();
        expansion_pending_ = false;
        question_.reset(); question_result_.clear(); question_wake_ = false;
    }
    if (presentation_ || question_work_ || !feed_.ready() || outbound_.size() > 54) return;
    if (!feed_.pending().empty()) submit("game.event");
    else if (question_wake_) submit("question.result");
    else if (awaiting_delegation_) submit("main.delegation");
    else if (const auto wake = autonomous_wake(); !wake.empty()) submit(wake);
}

// 借出最新角色状态，世界字符串只在本次 ABI 调用内有效。
dfy_v2_partner_status PartnerLoop::status() const noexcept {
    return {phase_,feed_.ready(),session_,feed_.cursor(),feed_.departed(),settled_,view(feed_.world()),view(feed_.owner())};
}
}
