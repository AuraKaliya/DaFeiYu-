#include "partner_loop.hpp"
#include "partner_effect.hpp"
#include "partner_knowledge.hpp"
#include <dafeiyu/tool_abi_v2.h>
#include <algorithm>
#include <array>

namespace dfy::mc {
namespace {
using Json = nlohmann::json;

// 借出仅本次 ABI 调用使用的字符串。
constexpr dfy_v2_string_view view(std::string_view bytes) noexcept { return {bytes.data(),bytes.size()}; }
constexpr std::array delegated_skills{view("mc.companion/observe"),view("mc.companion/reply"),view("mc.companion/collect")};
constexpr auto instructions = view("Minecraft 同场规则\n"
    "events 是新事实和玩家原话，recentDialogue 是已处理原话与已发送的发言。"
    "task 保存共同目标和原话指向。挖‘这格’用 observe 的 message 与 focus=hit，找洞口用 focus=openings；"
    "按原消息号和 candidate 绑定。缺信息时优先补一次针对性观察。"
    "玩家走开后沿用已绑定格。原位置不可用且玩家请你过来时，先登记 target.approach，移动到玩家，"
    "到达后用 observe 的 self 和 verify 读空格，再用 observation.id 和 cells 绑定原话。"
    "观察仍无法区分且会改变做法时，用 ask_player 自然问一句；玩家给了确切指向后，在 dig_block 带上 pendingQuestion.item 作为 resolves。\n"
    "先用一次简要观察读现场，再提交明确动作。跟随和 reply 可同批给出，跟随只开一次，"
    "用 following.work 停止，临时动作结束后由程序恢复。"
    "挖指定单格用 dig_block，按背包缺口从地上取材用 gather_ground，补已绑定的洞口用 fill_target。"
    "有依赖的短任务用 queue_actions 的 plan 写明节点和 after，由程序等待回执并推进。新原话可沿用已绑定目标并提交替代计划。"
    "只展开需要的类别。掉落规则可查 knowledge，库存和逐格变化以 workSettlement、planSettlements 或实际观察为准。"
    "accepted 和 queued 表示已接单。需要当场接话时同批提交 reply。程序等到实际回执后继续任务。\n"
    "行动、观察和说话可以独立继续。有人搭话就接他的原话；行动没结束时只谈已知进展。"
    "reply 发给玩家，处理原话时普通最终文本也可发一次，已经发过或仅需内部结算时用空文本。"
    "没有新消息时，主动发言用 reply，只在玩家关心的进展或受阻发生时发。入场无需固定问候。"
    "明确需要授权时才用动作的 approval，聊天只带实际问题。等答复时照常交谈和观察。"
    "玩家交来材料时，核对库存，撤回已无须执行的采集确认，续做原目标。\n");
constexpr auto review_instructions = view("根据当前分工、现场及新事件决定是否提前停下列出的原动作。"
    "只回答 stop 或 continue。回答 stop 才会请求中止。这个判断不向玩家发言，已经发生的动作效果继续保留。");
constexpr auto delegation_instructions = view("delegation 是宿主转发的委托。核对现场后按原编号结算。"
    "委托中的采集须征求当前玩家批准，默认由本体选择渠道。回复工具用于实际向玩家说话，最后的文本是给本体的报告。");

// 将基本观察、沟通和发现能力作为同场陪玩的初始契约。
bool base_category(SkillCategory category) noexcept {
    return category == SkillCategory::control || category == SkillCategory::observation ||
        category == SkillCategory::communication;
}

// 将最终文本按发言契约整理为游戏聊天行，显式内部结算留在原回合。
// 步骤：识别内部结算 → 整理控制字符和空白 → 按聊天长度裁剪到完整字符。
std::string player_reply(std::string_view answer) {
    const auto first = answer.find_first_not_of(" \t\r\n");
    if (first == answer.npos) return {};
    answer.remove_prefix(first);
    if (answer.front() == '{') {
        const auto envelope = Json::parse(answer,nullptr,false);
        if (envelope.is_object() && envelope.contains("internal")) return {};
    }
    std::string text(answer);
    std::replace_if(text.begin(),text.end(),[](unsigned char byte) { return byte < ' '; },' ');
    const auto start = text.find_first_not_of(' ');
    if (start == text.npos) return {};
    text.erase(0,start);
    unsigned units = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const auto byte = static_cast<unsigned char>(text[index]);
        if ((byte & 0xc0) == 0x80) continue;
        units += byte >= 0xf0 ? 2 : 1;
        if (units > 220) { text.resize(index); break; }
    }
    text.erase(text.find_last_not_of(' ')+1);
    return text;
}
}

// 由玩家入口显式启用，插件加载本身不读取游戏或提交模型。
// 步骤：协商子会话及通信接口 → 建立定向地址 → 开启配对玩家事件循环。
uint32_t PartnerLoop::listen(dfy_v2_query_interface_fn host, const BridgeConfig& config) {
    if (phase_ == DFY_V2_PARTNER_LISTENING) return DFY_V2_OK;
    if (phase_ == DFY_V2_PARTNER_STOPPING) return DFY_V2_BUSY;
    if (config.key.empty()) return DFY_V2_UNSUPPORTED;
    const void* api = nullptr;
    auto version = DFY_V2_AGENT_PERSONA_VERSION;
    auto result = host(view(DFY_V2_AGENT_INTERFACE),version,&api,nullptr);
    if (result == DFY_V2_UNSUPPORTED) {
        version = DFY_V2_AGENT_VERSION;
        result = host(view(DFY_V2_AGENT_INTERFACE),version,&api,nullptr);
    }
    if (result || !api) return result ? result : DFY_V2_UNSUPPORTED;
    const auto& agent = *static_cast<const dfy_v2_agent_api*>(api);
    if (agent.header.struct_size != sizeof(agent) || agent.header.interface_version != version ||
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
    motion_.reset();
    follow_.reset();
    queued_.clear();
    replacing_motion_ = false;
    guarded_actions_.clear();
    settlements_.clear();
    plan_.reset();
    plan_receipts_.clear();
    expanded_.reset();
    expansion_pending_ = false;
    phase_ = DFY_V2_PARTNER_CLOSED;
    agent_ = nullptr;
    mail_ = nullptr;
    mail_text_ = nullptr;
    refs_ = nullptr;
    confirmation_.reset();
    approval_result_ = nullptr;
    presentation_.reset();
    awaiting_delegation_.reset();
    active_delegation_.reset();
    last_task_ = 0;
    outbound_.clear();
    release_.reset();
    while (action_count_) actions_[--action_count_] = {};
    final_reply_.reset();
    reply_requested_ = false;
    session_world_.clear();
    session_ = request_ = order_ = settled_ = 0;
    review_ = {};
    events_.clear();
    recent_events_.clear();
    dialogue_.clear();
    previous_.clear();
    reviewed_ = 0;
    stopped_work_ = false;
    question_.reset(); question_work_.reset(); question_result_.clear(); question_wake_ = false; question_outbound_.clear();
}

// 等待原子回合退出并结算该回合关联的实际发言。
// 步骤：核对请求并按需取消 → 等待游戏发言回执 → 记录原回合结算水位。
bool PartnerLoop::settle(const BridgeConfig& config, std::uint64_t instance, bool cancel) {
    if (!request_) return true;
    dfy_v2_agent_status state{};
    const auto result = agent_->status(session_,&state,nullptr);
    if (result == DFY_V2_INVALID_ARGUMENT && cancel) state.phase = DFY_V2_AGENT_CANCELLED;
    else if (result || state.request != request_) throw std::runtime_error("MC partner: original turn unavailable");
    if (state.phase == DFY_V2_AGENT_QUEUED || state.phase == DFY_V2_AGENT_RUNNING) {
        if (cancel) agent_->cancel(session_,request_,nullptr);
        return false;
    }
    const auto answer = state.answer.length ? std::string_view(state.answer.data,state.answer.length) : std::string_view{};
    if (!settle_reply(config,instance,answer,cancel || state.phase != DFY_V2_AGENT_SUCCEEDED)) return false;

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
        {"failed",state.phase == DFY_V2_AGENT_FAILED && permits(session_) && !cancel},
        {"outcome",cancel ? (feed_.departed() ? "player_left" : "cancelled") :
            state.phase == DFY_V2_AGENT_SUCCEEDED ? "completed" : "unavailable"}});
    request_ = 0;
    return true;
}

// 把玩家原话回合的纯文本接回原游戏回复通道，一个回合至多启动一次。
// 步骤：核对发言归属与绑定 → 推进原回复票据 → 发布已验证的实际结果。
bool PartnerLoop::settle_reply(const BridgeConfig& config, std::uint64_t instance, std::string_view answer, bool cancel) {
    const auto work = "reply."+std::to_string(instance)+"."+std::to_string(session_)+"."+std::to_string(request_);
    if (!final_reply_) {
        if (cancel || reply_requested_ || active_delegation_ || !permits(session_) || !feed_.ready() ||
            feed_.departed() || events_.empty() || events_.back().at("type") != "player.message") return true;
        auto text = player_reply(answer);
        if (text.empty()) return true;
        const auto envelope = Json{{"context",{{"generation",session_world_},{"order",order_}}},
            {"arguments",{{"text",std::move(text)}}}}.dump();
        final_reply_ = std::make_unique<GameWork>(GameAction::partner,config,work,envelope,"reply");
    }

    std::optional<GameResult> result;
    try { result = final_reply_->poll(cancel || feed_.departed()); }
    catch (const std::exception&) { result = GameResult{false,GameEffect::unknown,{}}; }
    if (!result) return false;

    const auto effect = result->effect == GameEffect::observed ? DFY_V2_TOOL_APPLIED :
        result->effect == GameEffect::none ? DFY_V2_TOOL_NOT_APPLIED : DFY_V2_TOOL_UNKNOWN;
    if (result->payload.empty()) result->payload = R"({"status":"unconfirmed","sent":false})";
    record(session_,0,"reply",effect,work,std::move(result->payload));
    report_actions();
    final_reply_.reset();
    return true;
}

// 原会话空闲后释放旧游戏上下文，下一世界重新签发身份。
void PartnerLoop::retire() {
    if (session_) agent_->close(session_,nullptr);
    session_world_.clear();
    session_ = order_ = settled_ = 0;
    task_ = {};
    guarded_actions_.clear();
    settlements_.clear();
    plan_.reset();
    plan_receipts_.clear();
    expanded_.reset();
    expansion_pending_ = false;
    recent_events_.clear();
    dialogue_.clear();
    previous_.clear();
    question_.reset(); question_result_.clear(); question_wake_ = false;
    approval_result_ = nullptr;
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
    const bool autonomous = facts.empty() && !delegated && !answer && wake != "approval.result";
    const auto order = facts.empty() ? feed_.cursor() : facts.back().at("sequence").get<std::uint64_t>();
    Json input{{"player",feed_.owner()},{"generation",session_world_},{"events",facts},{"lastObservation",feed_.observation()},
        {"task",task_.context()},{"trigger",wake}};
    if (!dialogue_.empty() && !delegated) input["recentDialogue"] = dialogue_;
    if (feed_.scene_revision()) input["sceneRevision"] = feed_.scene_revision();
    if (motion_) {
        auto& active = input["activeMotion"];
        active = {{"work",motion_->work},{"skill",motion_->skill},{"order",motion_->order},
            {"status",motion_->stopping ? "stopping" : motion_->accepted ? "running" : "starting"}};
        if (!motion_->action->progress().empty()) active["progress"] = Json::parse(motion_->action->progress());
    }
    input["foreground"] = motion_ ? "active" : "standby";
    if (follow_) input["following"] = {{"work",follow_->id},{"segment",follow_->segment},
        {"status",follow_->pausing ? "pausing" : follow_->suspended ? "paused" : "following"}};
    if (!queued_.empty() && !delegated) {
        input["queuedActions"] = Json::array();
        for (const auto& step : queued_)
            input["queuedActions"].push_back({{"work",step.work},{"skill",step.skill},{"arguments",step.arguments}});
    }
    if (autonomous) input["recentEvents"] = recent_events_;
    if (!delegated && plan_) input["taskPlan"] = plan_context();
    if (!delegated && !plan_receipts_.empty()) input["planSettlements"] = plan_receipts_;
    const auto completed = delegated ? settlements_.cend() : ready_settlement();
    if (completed != settlements_.cend()) {
        const auto& receipts = completed->receipts;
        auto primary = receipts.size()-1;
        while (primary && !receipts.at(primary).contains("effect")) --primary;
        input["workSettlement"] = {{"invocation",completed->invocation},{"primary",primary},{"receipts",receipts}};
        auto& task = input.at("task");
        const auto latest = task.find("lastAction");
        if (latest != task.end() && std::ranges::any_of(receipts,[&](const auto& receipt) {
            return receipt.at("bridgeWork") == latest->at("bridgeWork");
        })) task.erase(latest);
    }
    if (!question_result_.is_null() && !question_result_.empty()) input["questionResult"] = question_result_;
    if (question_) input["pendingQuestion"] = {{"item",question_->item},{"question",question_->text},{"path",question_->path},
        {"options",question_->options}};
    if (confirmation_ && confirmation_->work && !delegated) {
        const auto& candidate = *confirmation_;
        input["pendingApproval"] = {{"item",candidate.request},{"order",candidate.order},{"skill",candidate.skill},
            {"arguments",candidate.arguments},{"question",candidate.question},{"status",candidate.approved_order ? "approved" : "waiting"}};
    }
    if (!delegated && !approval_result_.is_null()) input["approvalResult"] = approval_result_;
    if (delegated) input["delegation"] = {{"id",awaiting_delegation_->id},{"text",awaiting_delegation_->text}};
    if (previous_.is_object() && !previous_.empty()) input["previousActivity"] = previous_;
    if (feed_.gap()) input["eventGap"] = true;
    const auto prompt = input.dump();
    const auto binding = Json{{"generation",session_world_},{"order",order}}.dump();
    std::array<std::string,partner_skills.size()> ids;
    std::array<dfy_v2_string_view,partner_skills.size()> visible{};
    std::size_t count = 0;
    const auto enabled = scope();
    if (!delegated) for (const auto& skill : partner_skills) {
        if (skill.kind == SkillKind::confirmed || skill.name == "move" || (skill.name == "fill_target" && !task_.target_ready())) continue;
        if (skill.kind == SkillKind::motion_stop && !motion_ && !follow_) continue;
        if (skill.kind == SkillKind::approval_withdraw &&
            (!confirmation_ || !confirmation_->work || !confirmation_->decision.is_null())) continue;
        const auto category = skill.category;
        const bool direct = skill.name == "follow" || skill.name == "dig_block" || skill.name == "gather_ground";
        if (skill.kind != SkillKind::catalog && !direct && !base_category(category) &&
            !enabled.test(static_cast<std::size_t>(category))) continue;
        ids[count] = "mc.companion/"+std::string(skill.name);
        visible[count] = view(ids[count]);
        ++count;
    }
    const auto persona = !delegated && agent_->header.interface_version == DFY_V2_AGENT_PERSONA_VERSION ? DFY_V2_AGENT_PLAYER_FACING : 0;
    const dfy_v2_agent_input run{view(prompt),delegated ? delegation_instructions : instructions,view(binding),
        delegated ? delegated_skills.data() : visible.data(),
        static_cast<uint32_t>(delegated ? delegated_skills.size() : count),DFY_V2_AGENT_ALLOW_CHANGES | DFY_V2_AGENT_REPLACE_HISTORY | persona};
    if (agent_->submit(session_,&run,&request_,nullptr)) throw std::runtime_error("MC partner: cannot queue child turn");
    if (!delegated) {
        task_.submitted(!facts.empty() || answer,autonomous);
        question_wake_ = false;
        question_result_.clear();
        expansion_pending_ = false;
        if (completed != settlements_.cend()) settlements_.erase(completed);
        plan_receipts_.clear();
        approval_result_ = nullptr;
    }
    order_ = order;
    wait_for_work_ = false;
    follow_handoff_ = false;
    reply_requested_ = false;
    reviewed_ = 0;
    stopped_work_ = false;
    events_ = std::move(facts);
    if (!events_.empty()) recent_events_ = events_;
    if (delegated) { active_delegation_ = std::move(awaiting_delegation_); awaiting_delegation_.reset(); }
    for (const auto& fact : events_) {
        if (fact.at("type") == "player.message") remember_dialogue("player",fact.at("sequence"),fact.at("text"));
        publish("mc.partner.event",{{"generation",session_world_},{"event",fact}});
    }
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
    if (skill == "queue_actions") return queue_actions(invocation,args);
    if (skill == "ask_player") {
        auto question = open_question(invocation,args);
        reply_requested_ = true;
        if (question.at("status") == "recorded") wait_for_work_ = true;
        return question;
    }
    if (skill == "stop_motion") {
        const auto work = args.at("work").get<std::string>();
        if (follow_ && work == follow_->id && (!motion_ || motion_->skill != "follow")) {
            follow_.reset();
            publish("mc.partner.motion",{{"status","cancelled"},{"work",work},{"phase","paused"}});
            return {{"status","stopped"},{"work",work}};
        }
        if (!motion_ || (work != motion_->work && (!follow_ || motion_->skill != "follow" || work != follow_->id)))
            return {{"status","stale"}};
        if (motion_->plan_node) cancel_plan("stopped");
        clear_queue("stopped");
        if (motion_->skill == "follow" && follow_) follow_->pausing = false;
        motion_->stopping = true;
        return {{"status","stopping"},{"work",work}};
    }
    if (skill == "withdraw_confirmation") {
        if (!confirmation_ || !confirmation_->work || !confirmation_->decision.is_null() || args.at("item") != confirmation_->request)
            return {{"status","stale"}};
        confirm_once("withdrawn","task");
        return {{"status","withdrawn"},{"item",confirmation_->request}};
    }
    if (skill == "skills") return discover(args);
    if (skill == "knowledge") return lookup_knowledge(args);
    const auto target = args.find("target");
    const auto anchor = target != args.end() && target->contains("message") ?
        feed_.message_scene(target->at("message").get<std::uint64_t>()) : nullptr;
    const auto prior_scope = scope();
    const auto player_order = !events_.empty() && events_.back().at("type") == "player.message" ? order_ : 0;
    const auto prior_revision = task_.revision();
    auto result = task_.update(args,feed_.observation(),feed_.scene_revision(),anchor,player_order);
    if (motion_ && motion_->skill == "fill_target" && task_.revision() != prior_revision &&
        args.contains("target") && (args.at("target").empty() || args.at("target").value("replace",false)))
        motion_->stopping = true;
    expansion_pending_ |= (scope() & ~prior_scope).any();
    return result;
}

// 对原会话的放置附上任务身份，后续确认继续使用同一份命令。
bool PartnerLoop::bind_placement(std::uint64_t session, std::string& envelope, bool whole_target) const {
    if (session != session_) return !whole_target;
    if (!task_.target_ready()) return false;
    auto input = Json::parse(envelope);
    auto& arguments = input.at("arguments");
    if (!permits(session) || (whole_target && delegated(session)) || input.at("context").at("generation") != session_world_ ||
        input.at("context").at("order") != order_ ||
        !(whole_target ? bind_fill(arguments) : task_.bind_placement(arguments))) return false;
    envelope = input.dump();
    return true;
}

// 拷贝固定目标的格子，逐格沿用单格放置的来源与确认规则。
// 步骤：限定单格材料 → 核对全部格的约束 → 固定顺序和原话来源。
bool PartnerLoop::bind_fill(Json& arguments) const {
    const auto& material = arguments.at("block").get_ref<const std::string&>();
    if (!task_.target_ready() || material.ends_with("_door")) return false;
    auto cells = task_.target_cells();
    std::ranges::stable_sort(cells.get_ref<Json::array_t&>(),[](const auto& a, const auto& b) { return a.at("y") < b.at("y"); });
    Json placement;
    for (const auto& cell : cells) {
        placement = cell;
        placement["block"] = material;
        if (requires_approval("place",placement) || !task_.bind_placement(placement)) return false;
    }
    arguments["cells"] = std::move(cells);
    arguments["sourceMessage"] = placement.at("sourceMessage");
    arguments["taskRevision"] = placement.at("taskRevision");
    if (placement.contains("sourceMode")) arguments["sourceMode"] = placement.at("sourceMode");
    return true;
}

// 给采掘原命令附上当前任务的取材约束。
void PartnerLoop::bind_collection(std::uint64_t session, std::string& envelope) const {
    if (session != session_ || !permits(session)) return;
    auto input = Json::parse(envelope);
    task_.bind_collection(input.at("arguments"));
    envelope = input.dump();
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
        for (const auto& skill : partner_skills) if (skill.kind != SkillKind::confirmed && skill.name != "move" &&
            static_cast<std::size_t>(skill.category) == index)
            names.push_back(skill.name);
        if (names.empty() || base_category(static_cast<SkillCategory>(index))) next.reset(index);
        directory.push_back({{"category",categories[index]},{"available",!names.empty()},{"tools",std::move(names)}});
    }
    const bool changed = (next & ~scope()).any();
    expanded_ |= next;
    if (changed) {
        expansion_pending_ = true;
    }
    return {{"status",changed ? "scheduled" : "listed"},{"categories",std::move(directory)},
        {"wake",expansion_pending_ && task_.can_continue()}};
}

// 局内任务在锚点和确切目标就绪时获得对应的工具目录。
std::bitset<categories.size()> PartnerLoop::scope() const noexcept {
    auto enabled = expanded_;
    if (task_.has_target()) enabled.set(static_cast<std::size_t>(SkillCategory::movement));
    if (task_.target_ready()) enabled.set(static_cast<std::size_t>(SkillCategory::place));
    return enabled;
}

// 将已收原话或实际发言放入有界的对话窗口。
void PartnerLoop::remember_dialogue(std::string_view speaker, std::uint64_t order, const Json& text) {
    dialogue_.push_back({{"speaker",speaker},{"order",order},{"text",text}});
    if (dialogue_.size() > 8) dialogue_.pop_front();
}

// 新输入优先，断线、缺口和自主预算均可阻止调度下一决策。
std::string_view PartnerLoop::autonomous_wake() const {
    if (!feed_.ready() || !feed_.pending().empty() || feed_.gap() || feed_.departed()) return {};
    if (ready_settlement() != settlements_.cend()) return "motion.finished";
    if (plan_ && !plan_->active && !plan_receipts_.empty()) return "plan.finished";
    if (plan_ && plan_->active) return {};
    if (wait_for_work_) return {};
    if (expansion_pending_ && task_.can_continue()) return "skills.expanded";
    if (motion_) return {};
    return task_.trigger(feed_.observation(),feed_.scene_revision());
}

// 把原退出票据的效果与正文移动到本轮预留槽位。
void PartnerLoop::record(std::uint64_t session, std::uint64_t invocation, std::string_view skill, uint32_t effect,
    std::string work, std::string output) noexcept {
    if (!session_ || session != session_ || !request_) return;
    if (invocation && skill == "reply") reply_requested_ = true;
    if (action_count_ >= actions_.size()) return;
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
        if (!action.id) fact["source"] = "final_text";
        if (active_delegation_) fact["task"] = active_delegation_->id;
        if (!action.body.empty() && action.body.size() <= 8*1024) fact["result"] = Json::parse(action.body);
        else if (!action.body.empty()) fact["bodyOmitted"] = true;
        if (action.skill == "reply" && fact.contains("result") && fact["result"].value("sent",false) &&
            !fact["result"].value("reused",false) && fact["result"].contains("text"))
            remember_dialogue("companion",order_,fact["result"]["text"]);
        if (!active_delegation_) task_.record(fact);
        publish("mc.partner.action",fact);
        action = {};
    }
    action_count_ = 0;
}

// 移交已接纳的原工作和不可随新回合改写的关联身份。
void PartnerLoop::adopt_motion(std::uint64_t invocation, std::string_view skill, const std::string& work,
    std::string_view envelope, std::unique_ptr<GameWork>& action, bool wait) {
    if (motion_) throw std::logic_error("MC partner: motion already active");
    if (skill == "follow") {
        follow_ = Follow{order_,request_,invocation,1,work,Json::parse(envelope).at("arguments")};
        follow_handoff_ = true;
    }
    motion_.emplace(Motion{order_,session_,request_,invocation,session_world_,work,skill,std::move(action)});
    wait_for_work_ = wait_for_work_ || wait;
}

// 归档调用组的终态，保留多个游戏工作各自的回执。
void PartnerLoop::settle_work(std::uint64_t invocation, Json fact) {
    const auto group = std::ranges::find(settlements_,invocation,&WorkSettlement::invocation);
    if (group == settlements_.end()) settlements_.push_back({invocation,Json::array({std::move(fact)})});
    else group->receipts.push_back(std::move(fact));
}

// 跳过仍在执行的调用组，避免把中间状态呈现为结算。
std::deque<PartnerLoop::WorkSettlement>::const_iterator PartnerLoop::ready_settlement() const {
    return std::ranges::find_if(settlements_,[&](const auto& group) {
        return (!motion_ || motion_->invocation != group.invocation) &&
            std::ranges::find(queued_,group.invocation,&QueuedAction::invocation) == queued_.end();
    });
}

// 独立于模型回合轮询已接纳的动作，并记录原动作的真实终态。
// 步骤：发布接纳 → 处理停止条件并重读 → 归档归属和结果并唤醒。
void PartnerLoop::advance_motion(bool stopping) {
    if (stopping && (!motion_ || motion_->skill != "follow")) follow_.reset();
    if (!motion_) return;
    auto& motion = *motion_;
    Json fact{{"generation",motion.world},{"order",motion.order},{"session",motion.session},
        {"request",motion.request},{"invocation",motion.invocation},{"skill",motion.skill},{"bridgeWork",motion.work}};
    if (motion.skill == "follow" && follow_) {
        fact["work"] = follow_->id;
        fact["segment"] = follow_->segment;
    }
    motion.stopping = motion.stopping || stopping;
    std::optional<GameResult> result;
    try { result = motion.action->poll(motion.stopping,!motion.accepted); }
    catch (...) { result = GameResult{false,GameEffect::unknown,{}}; }

    if (result && result->accepted) { motion.accepted = true; result.reset(); }
    if (motion.accepted && !motion.announced) {
        publish("mc.partner.motion",{{"action",fact},{"status","accepted"}});
        motion.announced = true;
    }
    if (!result) {
        auto progress = motion.action->progress().empty() ? Json::object() : Json::parse(motion.action->progress());
        const auto phase = motion.stopping ? std::string("stopping") : progress.value("phase",std::string{});
        const auto observed = phase+":"+progress.value("terrain",std::string{});
        if (!phase.empty() && motion.last_phase != observed) {
            publish("mc.partner.motion",{{"action",fact},{"status",phase},{"progress",std::move(progress)}});
            motion.last_phase = observed;
        }
        return;
    }
    fact["effect"] = result->effect == GameEffect::observed ? DFY_V2_TOOL_APPLIED :
        result->effect == GameEffect::none ? DFY_V2_TOOL_NOT_APPLIED : DFY_V2_TOOL_UNKNOWN;
    fact["result"] = result->payload.empty() ? Json{{"status","unconfirmed"}} : Json::parse(result->payload);
    const auto& receipt = fact.at("result");
    const bool departed = receipt.at("status") == "unconfirmed";
    const bool paused = motion.skill == "follow" && follow_ && follow_->pausing && !stopping && !departed;
    if (motion.skill == "follow" && follow_) {
        fact["transition"] = paused ? "paused" : "exited";
        if (paused) { follow_->pausing = false; follow_->suspended = true; }
        else follow_.reset();
    }
    publish("mc.partner.action",fact);
    const bool verified = verified_action(receipt,motion.skill,motion.pickup);
    const bool failed = motion.queued && !motion.plan_node && !verified &&
        !(replacing_motion_ && receipt.at("status") == "interrupted");
    if (motion.plan_node) {
        if (!stopping) task_.record(fact);
        finish_plan_node(*motion.plan_node,verified,receipt);
    } else if (!stopping && !paused) {
        task_.record(fact);
        settle_work(motion.invocation,std::move(fact));
    }
    if (departed) {
        follow_.reset();
        cancel_plan("exit_unconfirmed");
        clear_queue("exit_unconfirmed");
    } else if (failed) clear_queue("prerequisite_failed",motion.invocation);
    replacing_motion_ = false;
    motion_.reset();
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
void PartnerLoop::advance(const BridgeConfig& config, std::uint64_t instance, PartnerWork work, bool body_busy) {
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
    const bool unavailable = closing || replaced || feed_.departed();
    advance_motion(unavailable);
    advance_confirmed_work(config,unavailable,body_busy);
    advance_plan(config,unavailable);
    advance_queue(config,unavailable,body_busy);
    review(work,unavailable);
    if (!settle(config,instance,unavailable)) return;
    if (closing) {
        if (motion_ || (plan_ && plan_->active) || review_.request || presentation_ || question_work_ ||
            !release_game(config,instance) || !outbound_.empty()) return;
        retire(); reset(); return;
    }
    if (replaced) {
        if (motion_ || (plan_ && plan_->active)) return;
        retire();
    }
    if (feed_.departed()) {
        if (plan_ && plan_->active) return;
        task_ = {};
        settlements_.clear();
        plan_.reset();
        plan_receipts_.clear();
        expanded_.reset();
        expansion_pending_ = false;
        dialogue_.clear();
        question_.reset(); question_result_.clear(); question_wake_ = false;
        approval_result_ = nullptr;
        if (feed_.ready() && !feed_.pending().empty()) {
            for (const auto& fact : feed_.pending()) publish("mc.partner.event",{{"generation",feed_.world()},{"event",fact}});
            settled_ = feed_.cursor();
            feed_.consumed();
        }
        return;
    }
    if (presentation_ || question_work_ || !feed_.ready() || outbound_.size() > 54) return;
    if (!feed_.pending().empty()) submit("game.event");
    else if (question_wake_) submit("question.result");
    else if (!approval_result_.is_null()) submit("approval.result");
    else if (awaiting_delegation_) submit("main.delegation");
    else if (const auto wake = autonomous_wake(); !wake.empty()) submit(wake);
}

// 借出最新角色状态，世界字符串只在本次 ABI 调用内有效。
dfy_v2_partner_status PartnerLoop::status() const noexcept {
    return {phase_,feed_.ready(),session_,feed_.cursor(),feed_.departed(),settled_,view(feed_.world()),view(feed_.owner())};
}
}
