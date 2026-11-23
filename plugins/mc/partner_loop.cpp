#include "partner_loop.hpp"
#include "partner_effect.hpp"
#include "partner_knowledge.hpp"
#include <dafeiyu/tool_abi_v2.h>
#include <algorithm>
#include <array>
#include <span>

namespace dfy::mc {
namespace {
using Json = nlohmann::json;

// 借出仅本次 ABI 调用使用的字符串。
constexpr dfy_v2_string_view view(std::string_view bytes) noexcept { return {bytes.data(),bytes.size()}; }
constexpr std::array delegated_skills{view("mc.companion/observe"),view("mc.companion/reply"),view("mc.companion/collect")};

struct Guidance {
    std::string_view purpose, key, text;
    bool applies = true;
};

constexpr std::array review_guidance{
    Guidance{"flowGuidance","workReview","结合当前分工、现场和新事件，判断列出的在途工作是否应停止。"},
    Guidance{"outputConstraints","decision","只输出 stop 或 continue。"}
};
constexpr std::array delegation_guidance{
    Guidance{"flowGuidance","delegation","按原委托编号和本轮现场判断可执行的步骤。"},
    Guidance{"outputConstraints","result","向委托方报告已核实的结果及受阻原因。"}
};

// 按用途渲染本次适用的领域指引，原话与现场事实由请求正文承载。
std::string render_guidance(std::string_view title, std::span<const Guidance> entries) {
    Json sections = Json::object();
    for (const auto& entry : entries)
        if (entry.applies) sections[entry.purpose][entry.key] = entry.text;
    return std::string(title)+"\n"+sections.dump();
}

constexpr std::string_view nearby_materials =
    "先用当前自身观察核对背包和缺口数量。指定对象沿原话查指向。可自选时从已许可范围选候选，"
    "确切采掘格用自身 verify 读回坐标和材质，脚下材质和可见列表可用来筛选。"
    "自然地面补库存用 gather_ground，填写所需总量并遵守材料、区域和支撑限制。"
    "需要合成或放置时，按实际入包和执行结果推进依赖。受阻时按具体回执补查会改变判断的条件，"
    "或另选已获准的候选。需要替代材料时只把依赖答复的步骤挂到追问。效果未知沿原工作回查。";

// 由原话或实际取材状态判定本轮是否相关。
// 步骤：核对当前指令提示 → 查看任务范围和实际采掘 → 检查正在交付的计划结果。
bool needs_materials_guidance(const Json& input, bool player_message) {
    if (player_message && read_dig_intent(input.at("events").back().at("text").get_ref<const std::string&>()).gathering_hint)
        return true;
    const auto& task = input.at("task");
    if (task.at("collection") == "ground" && task.at("next") != "done") return true;
    if (input.contains("activeMotion") && input.at("activeMotion").at("skill") == "collect") return true;
    if (const auto work = input.find("workSettlement"); work != input.end() &&
        work->at("receipts").at(work->at("primary").get<std::size_t>()).at("skill") == "collect") return true;
    const auto plan = input.find("taskPlan");
    return plan != input.end() && (plan->at("status") == "running" || input.contains("planSettlements")) &&
        std::ranges::any_of(plan->at("nodes"),[](const auto& node) {
            return node.at("skill") == "dig_block" || node.at("skill") == "gather_ground";
        });
}

// 由当前局内状态选取同场指引，具体技能参数由工具目录提供。
// 步骤：读取目标、证据和工作状态 → 声明各条指引的适用条件 → 按用途送入领域系统段。
std::string companion_instructions(const Json& input, bool player_message, bool actual_outcome, bool tools_for_gathering) {
    const auto& task = input.at("task");
    const bool has_goal = !task.at("goal").get_ref<const std::string&>().empty();
    const bool correcting = task.contains("pendingCorrection");
    const bool has_work = input.contains("activeMotion") || input.contains("taskPlan") ||
        input.contains("queuedActions") || input.contains("workSettlement") || input.contains("planSettlements");
    const std::array entries{
        Guidance{"flowGuidance","scene","结合新原话和当前现场，决定是否接话、观察、行动或等待。"},
        Guidance{"flowGuidance","task","沿原话和当前任务修订推进，给步骤写出已知依赖。复合委托若只提交准备步骤，用计划的 remainingGoal 保留尚未完成的用户目标。",has_goal || player_message},
        Guidance{"flowGuidance","planOutcome","计划结束时对照原话、剩余目标和实效。条件已具备就提交下一步。需要新证据就定向补查。现场变化使后续操作已无必要时，按最新事实结算。只承诺继续之前先登记实际后继。",
            input.contains("planSettlements")},
        Guidance{"flowGuidance","continuationBudget","本次自主推进额度已用完。按已核实的进展和剩余目标作结，等待玩家新消息。",
            input.contains("continuationBudgetExhausted")},
        Guidance{"flowGuidance","correction","按当前纠正重核目标和证据，沿用仍有效的原话引用。",correcting},
        Guidance{"flowGuidance","handoff","先核对旧工作实际退出与当前修订，再推进有冲突的步骤。",input.contains("taskHandoff")},
        Guidance{"flowGuidance","pendingQuestion","保留原待答事项与已知后继，依赖答复的步骤等待，获准的独立步骤可以推进。",
            input.contains("pendingQuestion")},
        Guidance{"flowGuidance","questionResult","根据事项的结算状态、所选目标和当前证据判断原步骤能否继续。",
            input.contains("questionResult")},
        Guidance{"flowGuidance","clarification","局内疑问用本地追问及动作范围。玩家用自然语言补足线索时，先核对原消息和候选，再用 task_update 的 resolves 结算该事项。仍有不确定的步骤维持等待。",
            player_message && input.contains("pendingQuestion") && input.at("pendingQuestion").contains("scope")},
        Guidance{"flowGuidance","planRejection","根据实际拒绝原因调整候选。修订机会耗尽时说明受阻。",input.contains("planRejection")},
        Guidance{"flowGuidance","previousPlanRejection","较早消息的计划退回原因只供修订原计划时查阅。先处理当前原话与最新实效。",
            input.contains("previousPlanRejection")},
        Guidance{"domainExperience","targetEvidence","按原话的对象和数量选择现场候选。开口的准星不唯一只说明视线，明确授权多个开口时用原观察的候选组。补查应缩小范围或核验选中格，需要线索时只问缺失的一项。",
            player_message || correcting || input.contains("questionResult")},
        Guidance{"domainExperience","nearbyMaterials",nearby_materials,tools_for_gathering && needs_materials_guidance(input,player_message)},
        Guidance{"domainExperience","followAttention","用实际距离、高差和可见性判断能否恢复跟随，或需要玩家协助。",
            input.at("trigger") == "follow.attention"},
        Guidance{"outputConstraints","gameConversation","用玩家认得的地点、物品和动作说话，省略内部编号和工具名。只需答话时直接输出文本。需要边做边说时用 reply；回复后本轮就结束可用 finish。",
            player_message || actual_outcome},
        Guidance{"outputConstraints","workOutcome","按最近核实的方块、库存、站位或投递回执说进展。跨轮状态先核对是否已恢复，再说明现场。已接纳的步骤仍待实际回执。",
            has_work || actual_outcome}
    };
    return render_guidance("Minecraft 同场规则",entries);
}

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
// 步骤：签发本次控制身份 → 协商子会话及通信接口 → 开启配对玩家事件循环。
uint32_t PartnerLoop::listen(dfy_v2_query_interface_fn host, BridgeConfig& config) {
    if (phase_ == DFY_V2_PARTNER_LISTENING) return DFY_V2_OK;
    if (phase_ == DFY_V2_PARTNER_STOPPING) return DFY_V2_BUSY;
    if (config.key.empty()) return DFY_V2_UNSUPPORTED;
    auto controller = new_controller();
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
    config.controller = std::move(controller);
    phase_ = DFY_V2_PARTNER_LISTENING;
    return DFY_V2_OK;
}

// 保存协作取消责任，下一步停止读取并等待在途工作退出。
void PartnerLoop::stop() noexcept { if (phase_ == DFY_V2_PARTNER_LISTENING) phase_ = DFY_V2_PARTNER_STOPPING; }

// 在宿主已撤销全部子会话工作后释放插件本地读口。
void PartnerLoop::reset() noexcept {
    feed_.clear();
    task_ = {};
    handoff_.reset();
    hit_queries_.clear();
    motion_.reset();
    follow_.reset();
    queued_.clear();
    replacing_motion_ = false;
    guarded_actions_.clear();
    settlements_.clear();
    plan_.reset();
    plan_receipts_.clear();
    plan_rejection_ = nullptr;
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
    attention_turn_.reset();
    reply_ = rejection_reply_ = {};
    question_requested_ = final_text_for_player_ = false;
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
    placement_choice_ = {};
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

    if (!active_delegation_ && (cancel || (!attention_turn_ && state.phase != DFY_V2_AGENT_SUCCEEDED))) {
        task_.disarm();
        expansion_pending_ = false;
    }
    if (!handoff_) awaiting_restarted_plan_ = false;
    if (cancel || autonomous_wake().empty()) settled_ = order_;
    if (!permits(session_) && !cancel) previous_ = Json{{"events",events_},{"stopRequested",stopped_work_}};
    if (active_delegation_) {
        const bool answered = !cancel && state.phase == DFY_V2_AGENT_SUCCEEDED && state.answer.length && state.answer.length <= 2048;
        const auto text = answered ? std::string(state.answer.data,state.answer.length) : std::string("伙伴未交付本次委托的文字答复，请查实际回执");
        publish("mc.partner.report",{{"generation",active_delegation_->world},{"id",active_delegation_->id},{"ok",answered},{"text",text}});
        active_delegation_.reset();
    } else {
        Json turn{{"generation",session_world_},{"order",order_},{"session",session_},{"request",request_},
            {"failed",state.phase == DFY_V2_AGENT_FAILED && permits(session_) && !cancel},
            {"outcome",cancel ? (feed_.departed() || feed_.departure() ? "player_left" : "cancelled") :
                state.phase == DFY_V2_AGENT_SUCCEEDED ? "completed" : "unavailable"}};
        const auto& reply = plan_repair_exhausted_ ? rejection_reply_ : reply_;
        if (reply.finish && reply.delivered) turn["closingReply"] = reply.work;
        publish("mc.partner.turn",turn);
    }
    request_ = 0;
    attention_turn_.reset();
    return true;
}

// 把玩家原话和实际任务结算的纯文本交给同一游戏回复通道。
// 步骤：核对发言归属与绑定 → 推进原回复票据 → 发布已验证的实际结果。
bool PartnerLoop::settle_reply(const BridgeConfig& config, std::uint64_t instance, std::string_view answer, bool cancel) {
    const auto work = "reply."+std::to_string(instance)+"."+std::to_string(session_)+"."+std::to_string(request_);
    if (!final_reply_) {
        const auto& reserved = plan_repair_exhausted_ ? rejection_reply_ : reply_;
        if (cancel || (!reserved.work.empty() && !reserved.unsent) || question_requested_ || !final_text_for_player_ || active_delegation_) return true;
        auto text = player_reply(answer);
        if (text.empty()) return true;
        if (feed_.recovering()) return false;
        if (!permits(session_) || !feed_.ready() || feed_.departed()) return true;
        const auto envelope = Json{{"context",{{"generation",session_world_},{"order",order_}}},
            {"arguments",{{"text",std::move(text)}}}}.dump();
        final_reply_ = std::make_unique<GameWork>(GameAction::partner,config,work,envelope,"reply");
    }

    std::optional<GameResult> result;
    try { result = final_reply_->poll(cancel || feed_.departed(),false,!feed_.recovering()); }
    catch (const std::exception&) { result = GameResult{false,GameEffect::unknown,{}}; }
    if (!result) return false;

    const auto effect = result->effect == GameEffect::observed ? DFY_V2_TOOL_APPLIED :
        result->effect == GameEffect::none ? DFY_V2_TOOL_NOT_APPLIED : DFY_V2_TOOL_UNKNOWN;
    record(session_,0,"reply",effect,work,std::move(result->payload));
    report_actions();
    final_reply_.reset();
    return true;
}

// 原会话空闲后释放旧游戏上下文，等待新的世界或玩家在场周期。
void PartnerLoop::retire() {
    if (session_) agent_->close(session_,nullptr);
    session_world_.clear();
    session_ = order_ = settled_ = 0;
    task_ = {};
    follow_.reset();
    handoff_.reset();
    awaiting_restarted_plan_ = false;
    attention_turn_.reset();
    guarded_actions_.clear();
    settlements_.clear();
    plan_.reset();
    plan_receipts_.clear();
    plan_rejection_ = nullptr;
    expanded_.reset();
    expansion_pending_ = false;
    events_.clear();
    recent_events_.clear();
    dialogue_.clear();
    previous_.clear();
    question_.reset(); question_result_.clear(); question_wake_ = false;
    placement_choice_ = {};
    approval_result_ = nullptr;
}

// 将按序收到的原始事实及绑定游戏数据送入同一子会话。
// 步骤：创建或复用上下文 → 定格原玩家及候选范围 → 接纳后消费事实。
void PartnerLoop::submit(std::string_view wake) {
    if (!session_) {
        auto world = feed_.world();
        if (agent_->open(&session_,nullptr)) throw std::runtime_error("MC partner: cannot open child session");
        session_world_ = std::move(world);
        session_binding_ = feed_.binding_revision();
    }
    auto facts = feed_.pending();
    const bool delegated = wake == "main.delegation";
    const bool answer = wake == "question.result";
    const bool autonomous = facts.empty() && !delegated && !answer && wake != "approval.result";
    const bool settlement_only = autonomous && !task_.can_continue();
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
        if (motion_->task) {
            active["taskId"] = motion_->task;
            active["taskRevision"] = motion_->revision;
        }
    }
    input["foreground"] = motion_ ? "active" : "standby";
    if (follow_) {
        const auto progress = motion_ && motion_->skill == "follow" ? input.at("activeMotion").value("progress",Json::object()) : Json::object();
        const auto phase = progress.value("phase","");
        const auto status = follow_->pausing ? "pausing" : follow_->suspended && !follow_->needs_route_check ? "paused" :
            phase == "waiting_player" ? "waiting_player" : phase == "blocked" || follow_->needs_route_check ? "blocked" : "following";
        input["following"] = {{"work",follow_->id},{"segment",follow_->segment},{"status",status}};
        if (!delegated && follow_->attention.episode()) input["followAttention"] = follow_attention_context();
    }
    if (!queued_.empty() && !delegated) {
        input["queuedActions"] = Json::array();
        for (const auto& step : queued_)
            input["queuedActions"].push_back({{"work",step.work},{"skill",step.skill},{"arguments",step.arguments}});
    }
    if (autonomous) input["recentEvents"] = recent_events_;
    if (settlement_only) input["continuationBudgetExhausted"] = true;
    if (!delegated && plan_ && (!task_.id() || plan_->revision == task_.revision())) input["taskPlan"] = plan_context();
    if (!delegated && handoff_) input["taskHandoff"] = handoff_->receipt.is_null() ?
        Json{{"work",handoff_->work},{"status","waiting_for_old_work"}} : handoff_->receipt;
    if (!delegated && !plan_receipts_.empty()) input["planSettlements"] = plan_receipts_;
    if (!delegated && plan_rejection_.is_object() && plan_rejection_.at("revision") == task_.revision())
        input[plan_rejection_.at("order") == order ? "planRejection" : "previousPlanRejection"] = plan_rejection_;
    const auto completed = delegated ? settlements_.cend() : ready_settlement();
    bool completed_dig = false;
    bool actual_outcome = wake == "follow.attention" ||
        (wake == "plan.finished" && plan_ && !plan_->active && plan_->cancellation.empty());
    if (completed != settlements_.cend()) {
        const auto& receipts = completed->receipts;
        auto primary = receipts.size()-1;
        while (primary && !receipts.at(primary).contains("effect")) --primary;
        input["workSettlement"] = {{"invocation",completed->invocation},{"primary",primary},{"receipts",receipts}};
        const auto& receipt = receipts.at(primary);
        const auto& status = receipt.at("result").at("status");
        actual_outcome |= wake == "motion.finished" && (status == "done" || status == "blocked") && receipt.at("skill") != "follow";
        completed_dig = facts.empty() && !task_.correcting() && receipt.value("pointedDig",false) &&
            verified_action(receipt.at("result"),"collect",{});
        auto& task = input.at("task");
        const auto latest = task.find("lastAction");
        if (latest != task.end() && std::ranges::any_of(receipts,[&](const auto& receipt) {
            return receipt.at("bridgeWork") == latest->at("bridgeWork");
        })) task.erase(latest);
    }
    if (!question_result_.is_null() && !question_result_.empty()) input["questionResult"] = question_result_;
    if (question_) {
        input["pendingQuestion"] = {{"item",question_->item},{"question",question_->text},{"path",question_->path},
            {"sourceMessage",question_->order},{"originalText",question_->origin},
            {"options",question_->options},{"status",question_->expired ? "expired" : "waiting"}};
        if (const auto* opening = task_.opening_observation(question_->order))
            input["pendingQuestion"]["openingObservation"] = *opening;
    }
    if (question_ && !question_->after_pickup.is_null()) {
        input["pendingQuestion"]["afterPickup"] = question_->after_pickup;
    }
    if (question_ && !question_->scope.is_null()) {
        input["pendingQuestion"]["scope"] = question_->scope;
    }
    if (question_ && placement_choice_pending() && question_->item == placement_choice_.item)
        input["pendingQuestion"]["placementOptions"] = placement_choice_.options;
    if (placement_choice_.revision == task_.revision() && !placement_choice_.rejected.empty())
        input["placementExcluded"] = placement_choice_.rejected;
    if (confirmation_ && confirmation_->work && !delegated) {
        const auto& candidate = *confirmation_;
        input["pendingApproval"] = {{"item",candidate.request},{"order",candidate.order},{"skill",candidate.skill},
            {"arguments",candidate.arguments},{"question",candidate.question},{"status",candidate.approved_order ? "approved" : "waiting"}};
    }
    if (!delegated && !approval_result_.is_null()) input["approvalResult"] = approval_result_;
    if (delegated) input["delegation"] = {{"id",awaiting_delegation_->id},{"text",awaiting_delegation_->text}};
    if (previous_.is_object() && !previous_.empty()) input["previousActivity"] = previous_;
    if (feed_.gap()) input["eventGap"] = true;
    const bool player_message = !facts.empty() && facts.back().at("type") == "player.message";
    const auto prompt = input.dump();
    const auto binding = Json{{"generation",session_world_},{"order",order}}.dump();
    std::array<std::string,partner_skills.size()> ids;
    std::array<dfy_v2_string_view,partner_skills.size()> visible{};
    std::size_t count = 0;
    bool observe_visible = delegated, plan_visible = false;
    std::bitset<partner_skills.size()> disclosed;
    const auto enabled = scope();
    const bool completed_route = wake == "plan.finished" && task_.navigation() && plan_ && !plan_->active &&
        plan_->revision == task_.revision() && plan_->nodes.size() == 1 && plan_->nodes.front().skill == "route" &&
        plan_->nodes.front().state == PlanNode::State::done;
    const bool completed_pickup_plan = wake == "plan.finished" && plan_ && !plan_->active && plan_->pickup_question;
    if (!delegated) for (const auto& skill : partner_skills) {
        if ((completed_dig || completed_route || completed_pickup_plan || settlement_only) && skill.name != "reply") continue;
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
        disclosed.set(&skill-partner_skills.data());
        ++count;
        observe_visible |= skill.name == "observe";
        plan_visible |= skill.kind == SkillKind::action_queue;
    }
    const auto instructions = delegated ? render_guidance("Minecraft 委托规则",delegation_guidance) :
        companion_instructions(input,player_message,actual_outcome,observe_visible && plan_visible);
    const auto persona = !delegated && agent_->header.interface_version == DFY_V2_AGENT_PERSONA_VERSION ? DFY_V2_AGENT_PLAYER_FACING : 0;
    const dfy_v2_agent_input run{view(prompt),view(instructions),view(binding),
        delegated ? delegated_skills.data() : visible.data(),
        static_cast<uint32_t>(delegated ? delegated_skills.size() : count),DFY_V2_AGENT_ALLOW_CHANGES | DFY_V2_AGENT_REPLACE_HISTORY | persona};
    if (agent_->submit(session_,&run,&request_,nullptr)) throw std::runtime_error("MC partner: cannot queue child turn");
    visible_skills_ = disclosed;
    settling_plan_ = wake == "plan.finished" && actual_outcome ? plan_->id : 0;
    attention_turn_.reset();
    if (wake == "follow.attention") attention_turn_ = AttentionTurn{follow_->id,follow_->attention.episode()};
    if (!delegated) {
        if (follow_) follow_->attention_pending = false;
        if (handoff_ && !handoff_->receipt.is_null() && handoff_->receipt.at("status") != "old_work_exit_unconfirmed")
            handoff_.reset();
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
    plan_failures_ = 0;
    plan_repair_exhausted_ = false;
    plan_reply_step_ = 0;
    repair_failures_.clear();
    if (!facts.empty()) hit_queries_.clear();
    reply_ = rejection_reply_ = {};
    question_requested_ = false;
    final_text_for_player_ = !delegated && (actual_outcome || (!facts.empty() && facts.back().at("type") == "player.message"));
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
    if (session != session_) return true;
    return phase_ == DFY_V2_PARTNER_LISTENING && feed_.execution_ready() && feed_.world() == session_world_ &&
        feed_.binding_revision() == session_binding_ &&
        !feed_.departed() && !feed_.departure() &&
        feed_.pending().empty() && !feed_.gap() &&
        (!attention_turn_ || (follow_ && follow_->id == attention_turn_->work &&
            follow_->attention.episode() == attention_turn_->episode && follow_->attention.active()));
}

// 最后一次模型请求可读取原拒绝，继续循环必须等新的玩家或现场输入。
bool PartnerLoop::permits_model_step(std::uint64_t session, std::uint64_t step) noexcept {
    if (session != session_ || !plan_repair_exhausted_) return true;
    if (!plan_reply_step_) plan_reply_step_ = step;
    return step == plan_reply_step_;
}

// 只有处理当前世界的原子会话能修改局内状态，旧事件不能更新任务或展开范围。
Json PartnerLoop::local_skill(std::uint64_t session, std::uint64_t invocation, std::string_view skill, std::string_view envelope,
    bool body_busy) {
    if (session != session_ || !request_ || active_delegation_ || !permits(session) || feed_.departed() || !feed_.ready())
        throw std::invalid_argument("MC partner: task owner unavailable");
    const auto input = Json::parse(envelope);
    const auto& source = input.at("context");
    if (source.at("generation") != session_world_ || source.at("order") != order_)
        throw std::invalid_argument("MC partner: stale task context");
    const auto& args = input.at("arguments");
    if (skill == "queue_actions") {
        if (args.at("operation") == "plan") {
            if (plan_repair_exhausted_) return plan_rejection_.at("result");
            return report_plan_attempt(invocation,queue_actions(invocation,args));
        }
        auto result = queue_actions(invocation,args);
        if (result.at("status") == "invalid_plan")
            limit_repair("plan:"+result.value("reason",std::string{}));
        return result;
    }
    if (skill == "ask_player") {
        auto question = open_question(invocation,args);
        if (question.at("status") == "recorded" || question.at("status") == "waiting") {
            question_requested_ = true;
            wait_for_work_ |= question_->scope.is_null();
        }
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
    const auto operation = args.value("operation","");
    if (body_busy) return {{"status","busy"},{"reason","foreground_admitting"}};
    if (operation == "start_task" || operation == "restart_task") {
        return switch_task(args);
    }
    if (operation == "begin_correction") {
        if (task_.navigation()) return {{"status","invalid_target"},{"reason","task_switch_required"}};
        if (events_.empty() || events_.back().at("type") != "player.message" || args.at("message") != order_)
            return {{"status","stale_correction"}};
        auto result = task_.begin_correction(events_.back(),feed_.observation());
        if (result.at("status") == "correction_recorded") {
            if (waits_for_player(session_) && question_->order < order_) complete_question("superseded",{});
            cancel_plan("task_changed");
            clear_queue("task_changed");
            if (motion_ && !motion_->plan_node) hold_task_motion();
            if (confirmation_ && confirmation_->work) confirm_once("withdrawn","task");
        }
        return result;
    }
    const bool committing = args.value("operation","") == "commit_correction";
    if (committing && motion_ && motion_->stopping && motion_->skill != "follow")
        return {{"status","waiting_for_old_work"},{"work",motion_->work}};
    const auto target = args.find("target");
    const auto anchor = target != args.end() && target->contains("message") ?
        feed_.message_scene(target->at("message").get<std::uint64_t>()) : nullptr;
    const auto prior_scope = scope();
    const auto player_order = !events_.empty() && events_.back().at("type") == "player.message" ? order_ : 0;
    const auto resolves = args.value("resolves",std::uint64_t{});
    const bool waiting = waits_for_player(session_);
    const bool supports_placement = waiting && (question_->scope.is_null() || question_->scope.at("skill") == "place");
    const bool scoped_answer = resolves && waiting && resolves == question_->item && player_order > question_->order &&
        question_->presented && !question_->expired && std::chrono::steady_clock::now() < question_->local_answer_deadline &&
        !question_->scope.is_null() && question_->after_pickup.is_null() && !placement_choice_pending() &&
        (target == args.end() || (question_->scope.at("skill") == "place" && target->value("message",std::uint64_t{}) == question_->order)) &&
        (!args.contains("collection") || question_->scope.at("skill") == "collect");
    const bool correction_answer = committing && supports_placement && question_->scope.is_null() &&
        resolves == question_->item && player_order > question_->order;
    if (resolves && !scoped_answer && !correction_answer)
        return {{"status","invalid_target"},{"reason","question_resolution_mismatch"}};
    const bool answers_question = scoped_answer || correction_answer;
    const bool independent_target = waiting && !supports_placement && !committing && !args.contains("collection") &&
        args.at("goal").get_ref<const std::string&>() == task_.goal();
    if (placement_choice_pending() && !task_.correcting() &&
        (committing || target != args.end() || args.value("collection",std::string(task_.collection())) != task_.collection() ||
            args.at("goal").get_ref<const std::string&>() != task_.goal()))
        return {{"status","waiting_for_player"},{"item",placement_choice_.item}};
    if (waiting && !independent_target && (committing || target != args.end() || args.contains("collection") ||
        args.at("goal").get_ref<const std::string&>() != task_.goal()) &&
        (player_order <= question_->order || (committing && !answers_question)))
        return {{"status","waiting_for_player"},{"item",question_->item}};
    const auto prior_revision = task_.revision();
    auto result = task_.update(args,feed_.observation(),feed_.scene_revision(),anchor,player_order);
    if (answers_question && result.at("status") == "updated") {
        question_->answer_order = player_order;
        complete_question("resolved",events_.back().at("text").get<std::string>());
        if (scoped_answer) {
            result["resolvedQuestion"] = question_result_;
            auto fact = question_result_;
            fact["generation"] = session_world_;
            fact["order"] = player_order;
            publish("mc.partner.question",fact);
        }
    }
    if (task_.revision() != prior_revision) {
        if (independent_target) question_->task_revision = task_.revision();
        cancel_plan("task_changed");
        clear_queue("task_changed");
        settlements_.clear();
        if (confirmation_ && confirmation_->work) confirm_once("withdrawn","task");
        hold_task_motion();
    }
    expansion_pending_ |= (scope() & ~prior_scope).any();
    return result;
}

// 标记旧前台工作并保留原有交接，实际退出由其工作票据确认。
void PartnerLoop::hold_task_motion() {
    if (!motion_ || motion_->skill == "follow") return;
    if (!handoff_ || handoff_->work != motion_->work) handoff_ = Handoff{motion_->work};
    motion_->stopping = true;
}

// 登记新原话并撤下旧计划，已发送动作仍由原工作负责收尾。
// 步骤：校验新的玩家指令 → 撤销旧节点和追问 → 等待原前台工作的真实退出。
Json PartnerLoop::switch_task(const Json& args) {
    if (events_.empty() || events_.back().at("type") != "player.message" || args.value("message",std::uint64_t{}) != order_)
        return {{"status","stale"}};
    if (handoff_ && !handoff_->receipt.is_null() && handoff_->receipt.at("status") == "old_work_exit_unconfirmed")
        return handoff_->receipt;
    const auto result = task_.switch_task(args,events_.back());
    if (result.at("status") != "updated") return result;

    if (question_ && !question_->approval) complete_question("superseded",{});
    if (confirmation_ && confirmation_->work) confirm_once("withdrawn","task");
    cancel_plan("task_changed");
    clear_queue("task_changed");
    settlements_.clear();
    plan_receipts_.clear();
    if (motion_ && motion_->skill != "follow") {
        awaiting_restarted_plan_ = args.at("operation") == "restart_task" && motion_->plan_node.has_value();
        hold_task_motion();
    } else {
        awaiting_restarted_plan_ = false;
        handoff_.reset();
    }
    return result;
}

// 限制当前指向的后台观察次数，并在达到上限后阻止同类循环。
// 步骤：找到指定原话 → 扣除观察额度或保留一次领到反馈的机会。
bool PartnerLoop::admit_hit_observation(std::uint64_t session, const Json& args) {
    const auto focus = args.value("focus","");
    if (session != session_ || active_delegation_ || (focus != "hit" && focus != "objects") || !args.contains("message")) return true;
    const auto message = args.at("message").get<std::uint64_t>();
    auto& used = hit_queries_[message];
    if (used++ < 2) return true;
    limit_repair("observe_hit_budget");
    return false;
}

// 在原回合接纳发言时登记工作和可选的结束条件。
bool PartnerLoop::reserve_reply(std::uint64_t session, std::string_view work, bool finish) {
    if (session != session_ || active_delegation_) return true;
    auto& reserved = plan_repair_exhausted_ ? rejection_reply_ : reply_;
    if (!reserved.work.empty()) return false;
    reserved.work = work;
    reserved.finish = finish;
    return true;
}

// 结尾发言只有读回已发送才可以让出下一次送模，不影响正在归还的同批工具。
bool PartnerLoop::should_yield_model(std::uint64_t session) const noexcept {
    const auto& reply = plan_repair_exhausted_ ? rejection_reply_ : reply_;
    return session == session_ && (wait_for_work_ || (reply.finish && reply.delivered));
}

// 对原会话的放置附上任务身份，后续确认继续使用同一份命令。
// 步骤：核对回合和来源 → 按动作绑定目标 → 保存可下发的命令。
std::string_view PartnerLoop::bind_placement(std::uint64_t session, std::string_view skill, std::string& envelope) const {
    if (session != session_) {
        if (skill == "fill_target") return "placement_context_stale";
        return {};
    }
    auto input = Json::parse(envelope);
    const auto& context = input.at("context");
    if (!permits(session) || (skill == "fill_target" && delegated(session)) ||
        context.at("generation") != session_world_ || context.at("order") != order_) return "placement_context_stale";

    if (const auto reason = bind_placement_target(skill,input.at("arguments")); !reason.empty()) return reason;
    envelope = input.dump();
    return {};
}

// 让直调、顺序队列和计划使用同一放置范围及来源。
// 步骤：检查当前任务目标 → 单次放置核对占用格，整区填充展开已有目标 → 固定下发参数。
std::string_view PartnerLoop::bind_placement_target(std::string_view skill, Json& arguments) const {
    if (skill != "place" && skill != "fill_target") return {};
    if (!task_.target_ready()) return "target_unbound";
    if (skill == "place") {
        if (!task_.bind_placement(arguments)) return "placement_outside_target";
        return {};
    }

    if (arguments.at("block").get_ref<const std::string&>().ends_with("_door")) return "invalid_fill_material";
    auto cells = task_.target_cells();
    std::ranges::stable_sort(cells.get_ref<Json::array_t&>(),[](const auto& a, const auto& b) { return a.at("y") < b.at("y"); });
    auto placement = cells.front();
    placement["block"] = arguments.at("block");
    if (!task_.bind_placement(placement)) return "placement_outside_target";

    arguments["cells"] = std::move(cells);
    arguments["sourceMessage"] = placement.at("sourceMessage");
    arguments["taskRevision"] = placement.at("taskRevision");
    if (placement.contains("sourceMode")) arguments["sourceMode"] = placement.at("sourceMode");
    return {};
}

// 为精确采掘、普通采集和补库存准备同一执行器的参数。
// 步骤：按技能固定采掘量及来源 → 为普通取材绑定任务范围和保护格。
void PartnerLoop::bind_collection(std::string_view skill, Json& args) const {
    if (skill == "dig_block") {
        args["count"] = 1;
        args["source"] = "visible";
    } else if (skill == "gather_ground") {
        args["count"] = args.at("requiredStock");
        args["source"] = "ground";
    }

    // 显式拆改保留独立的确认门槛，由游戏侧核对已建对象及其支撑。
    if (!args.value("editBuilt",false)) task_.bind_collection(args);
}

// 返回动作已知的目标约束或执行前需要复核的条件。
// 步骤：核对放置绑定 → 检查当前计划的合成依赖 → 返回尚缺的实效。
Json PartnerLoop::skill_conditions(const PartnerAction& action) const {
    Json conditions{{"status","check_on_execution"},{"requirements",action.plan_requirements}};
    if (action.category == SkillCategory::place && !task_.target_ready()) conditions["status"] = "target_unbound";
    if (action.name != "craft" || !plan_ || !plan_->active || plan_->revision != task_.revision()) return conditions;

    for (const auto& node : plan_->nodes) {
        if (node.skill != action.name || node.state != PlanNode::State::pending) continue;
        for (const auto parent : node.after) {
            const auto& prerequisite = plan_->nodes.at(parent);
            if (prerequisite.pickup.empty() || (prerequisite.state != PlanNode::State::pending &&
                prerequisite.state != PlanNode::State::running)) continue;
            conditions["status"] = "waiting_for_pickup";
            conditions["work"] = prerequisite.work;
            conditions["item"] = prerequisite.pickup;
            return conditions;
        }
    }
    return conditions;
}

// 输出分类和入口契约，计划节点详情用已有状态回答。
// 步骤：核对分类 → 按当前快照生成入口说明 → 仅在直调范围扩大时登记唤醒。
Json PartnerLoop::discover(const Json& arguments) {
    const auto wanted = arguments.value("categories",std::vector<std::string>{});
    if (wanted.size() > 3) throw std::length_error("MC skills: choose up to three categories");
    std::bitset<categories.size()> next;
    for (const auto& name : wanted) {
        const auto found = std::find(categories.begin(),categories.end(),name);
        if (found == categories.end())
            return {{"status","invalid_category"},{"category",name},{"validCategories",categories},{"planNodes",plan_skills()}};
        next.set(found-categories.begin());
    }
    const auto enabled = scope();
    const auto queue = std::ranges::find(partner_skills,"queue_actions",&PartnerSkill::name);
    const bool queue_visible = visible_skills_.test(queue-partner_skills.begin());
    std::map<std::string_view,Json> entries;
    for (const auto& skill : partner_skills) if (skill.kind != SkillKind::confirmed && skill.name != "move")
        entries[skill.name] = {{"skill",skill.name},{"implemented",true},
            {"direct",{{"tool",skill.name},{"visible",visible_skills_.test(&skill-partner_skills.data())}}}};

    Json directory = Json::array();
    for (std::size_t index = 0; index < categories.size(); ++index) {
        Json names = Json::array(), nodes = Json::array();
        for (const auto& skill : partner_skills) if (skill.kind != SkillKind::confirmed && skill.name != "move" &&
            static_cast<std::size_t>(skill.category) == index)
            names.push_back(skill.name);

        for (const auto& action : partner_actions) {
            if ((!action.plan && !action.sequence) || static_cast<std::size_t>(action.category) != index) continue;
            auto& entry = entries[action.name];
            entry["skill"] = action.name;
            entry["implemented"] = !action.executor.empty();
            if (action.plan) {
                nodes.push_back(action.name);
                entry["plan"] = {{"tool","queue_actions"},{"visible",queue_visible},{"conditions",skill_conditions(action)}};
                if (next.test(index)) entry["plan"]["arguments"] = action.plan_arguments;
            }
            if (action.sequence) {
                entry["sequence"] = {{"tool","queue_actions"},{"visible",queue_visible && sequence_available(action,enabled)},
                    {"conditions",skill_conditions(action)}};
                if (next.test(index)) entry["sequence"]["arguments"] = action.sequence_arguments;
            }
        }
        if (names.empty() || base_category(static_cast<SkillCategory>(index))) next.reset(index);
        Json category{{"category",categories[index]},{"available",!names.empty() || !nodes.empty()},{"tools",std::move(names)}};
        if (!nodes.empty()) category["planNodes"] = std::move(nodes);
        directory.push_back(std::move(category));
    }
    const bool changed = (next & ~enabled).any();
    expanded_ |= next;
    if (changed) {
        expansion_pending_ = true;
    }
    Json procedure{{"name","就近取材"},{"category","dig_break"}};
    if (std::ranges::find(wanted,"dig_break") != wanted.end()) procedure["guidance"] = nearby_materials;
    Json contracts = Json::array();
    for (auto& [name,entry] : entries) contracts.push_back(std::move(entry));
    return {{"status",changed ? "scheduled" : "listed"},{"categories",std::move(directory)},{"planNodes",plan_skills()},
        {"contracts",std::move(contracts)},
        {"procedures",Json::array({std::move(procedure)})},
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
    if (handoff_ && !handoff_->receipt.is_null() && handoff_->receipt.at("status") == "exited") return "task.ready";
    if (follow_ && follow_->attention_pending && (!motion_ || motion_->skill == "follow")) return "follow.attention";
    if (wait_for_work_) return {};
    if (waits_for_player(session_) && question_->scope.is_null() &&
        (!placement_choice_pending() || question_->item != placement_choice_.item)) return {};
    if (expansion_pending_ && task_.can_continue()) return "skills.expanded";
    if (motion_) return {};
    return task_.correcting() ? std::string_view{} : task_.trigger(feed_.observation(),feed_.scene_revision());
}

// 在新回合看见原工作的终态之前，阻止所有前台入口。
std::string_view PartnerLoop::front_barrier(std::uint64_t session) const noexcept {
    if (session != session_ || !session_) return {};
    if (plan_repair_exhausted_) return "plan_repair_exhausted";
    if (handoff_) {
        return handoff_->receipt.is_object() && handoff_->receipt.at("status") == "old_work_exit_unconfirmed" ?
            "old_work_exit_unconfirmed" : "waiting_for_old_work";
    }
    if (task_.correcting()) return "correction_pending";
    return {};
}

// 登记原发言的实效并保存退出票据。
// 步骤：确认属于本轮 → 为下一模型请求登记投递终态 → 保存回执。
void PartnerLoop::record(std::uint64_t session, std::uint64_t invocation, std::string_view skill, uint32_t effect,
    std::string work, std::string output) noexcept {
    if (!session_ || session != session_ || !request_) return;
    if (skill == "reply" && effect == DFY_V2_TOOL_APPLIED) {
        auto& reply = work == reply_.work ? reply_ : rejection_reply_;
        if (work == reply.work) reply.delivered = true;
    }
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
        if (!active_delegation_ && action.skill == "observe" && fact.contains("result")) {
            auto code = fact.at("result").value("reasonCode",std::string{});
            if (code.starts_with("observe_")) limit_repair(std::move(code));
        }
        if (action.skill == "reply") {
            const bool unsent = action.effect == DFY_V2_TOOL_NOT_APPLIED;
            if (action.work == reply_.work) reply_.unsent = unsent;
            if (action.work == rejection_reply_.work) rejection_reply_.unsent = unsent;
            if (action.body.empty()) fact["result"] = {{"status",unsent ? "blocked" : "unconfirmed"},
                {"sent",false},{"delivery",unsent ? "not_sent" : "unknown"}};
            if (fact.contains("result") && fact["result"].value("sent",false) &&
                !fact["result"].value("reused",false) && fact["result"].contains("text"))
                remember_dialogue("companion",order_,fact["result"]["text"]);
        }
        if (!active_delegation_) task_.record(fact);
        publish("mc.partner.action",fact);
        action = {};
    }
    action_count_ = 0;
}

// 同类参数错误可以修正一次，重复失败时保持现场任务等待。
void PartnerLoop::limit_repair(std::string code) {
    if (++repair_failures_[std::move(code)] < 2) return;
    task_.disarm();
    wait_for_work_ = true;
}

// 移交已接纳的原工作和不可随新回合改写的关联身份。
void PartnerLoop::adopt_motion(std::uint64_t invocation, std::string_view skill, const std::string& work,
    std::string_view envelope, std::unique_ptr<GameWork>& action, bool wait, bool pointed) {
    if (motion_) throw std::logic_error("MC partner: motion already active");
    if (skill == "follow") {
        follow_ = Follow{order_,request_,invocation,1,work,Json::parse(envelope).at("arguments")};
    }
    motion_.emplace(Motion{order_,session_,request_,invocation,session_world_,work,skill,std::move(action)});
    motion_->pointed = pointed;
    motion_->task = task_.id();
    motion_->revision = task_.revision();
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

// 将最新样本与逻辑跟随和当前桥接执行段关联。
Json PartnerLoop::follow_attention_context() const {
    const auto& follow = *follow_;
    const auto active = follow.attention.active();
    return {{"generation",session_world_},{"work",follow.id},{"segment",follow.segment},
        {"episode",follow.attention.episode()},{"status",active ? "raised" : "recovered"},
        {active ? "reason" : "previousReason",follow.attention.reason()},{"progress",follow.attention.evidence()}};
}

// 同一异常只请求一次模型判断，恢复更新可见状态与 Trace。
void PartnerLoop::track_follow(const Json& evidence, bool terminal) {
    if (!follow_) return;
    const auto signal = terminal ? follow_->attention.blocked(evidence) :
        follow_->attention.observe(evidence,follow_->arguments.value("distance",3));
    if (signal == FollowSignal::none) return;
    follow_->attention_pending = signal == FollowSignal::raised;
    publish("mc.partner.attention",follow_attention_context());
}

// 独立于模型回合轮询已接纳的动作，并记录原动作的真实终态。
// 步骤：复核尚未下发的变更 → 推进或停止原工作 → 归档归属和结果并唤醒。
void PartnerLoop::advance_motion(bool stopping) {
    if (stopping && (!motion_ || motion_->skill != "follow")) follow_.reset();
    if (!motion_) return;
    auto& motion = *motion_;
    Json fact{{"generation",motion.world},{"order",motion.order},{"session",motion.session},
        {"request",motion.request},{"invocation",motion.invocation},{"skill",motion.skill},{"bridgeWork",motion.work}};
    if (motion.task) fact["taskId"] = motion.task;
    if (motion.skill != "follow") fact["taskRevision"] = motion.revision;
    if (motion.skill == "follow" && follow_) {
        fact["work"] = follow_->id;
        fact["segment"] = follow_->segment;
    }
    if (motion.pointed) fact["pointedDig"] = true;
    motion.stopping = motion.stopping || stopping;
    std::optional<GameResult> result;
    try {
        if (!motion.stopping && !motion.action->attempted() &&
            (motion.skill == "collect" || motion.skill == "place" || motion.skill == "fill_target" || motion.skill == "craft")) {
            const auto question = action_question(motion.session,motion.skill,Json::parse(motion.action->arguments()).at("arguments"));
            if (!question.is_null()) result = GameResult{false,GameEffect::none,question.dump()};
        }
        if (!result) result = motion.action->poll(motion.stopping,!motion.accepted,!feed_.recovering());
    }
    catch (...) { result = GameResult{false,GameEffect::unknown,{}}; }

    if (result && result->accepted) { motion.accepted = true; result.reset(); }
    if (motion.accepted && !motion.announced) {
        publish("mc.partner.motion",{{"action",fact},{"status","accepted"}});
        motion.announced = true;
    }
    if (!result) {
        auto progress = motion.action->progress().empty() ? Json::object() : Json::parse(motion.action->progress());
        if (motion.skill == "follow" && follow_ && !motion.stopping && progress.value("phase","") != "blocked" &&
            progress.value("path","") == "reachable") follow_->needs_route_check = false;
        if (motion.skill == "follow" && !motion.stopping) track_follow(progress);
        const auto phase = motion.stopping ? std::string("stopping") : progress.value("phase",std::string{});
        const auto observed = phase+":"+progress.value("terrain",std::string{});
        const auto marker = progress.contains("checkpoint") ? observed+":"+progress.at("checkpoint").dump() : observed;
        if (!phase.empty() && motion.last_phase != marker) {
            publish("mc.partner.motion",{{"action",fact},{"status",phase},{"progress",std::move(progress)}});
            motion.last_phase = marker;
        }
        return;
    }
    fact["effect"] = result->effect == GameEffect::observed ? DFY_V2_TOOL_APPLIED :
        result->effect == GameEffect::none ? DFY_V2_TOOL_NOT_APPLIED : DFY_V2_TOOL_UNKNOWN;
    const bool handing_off = handoff_ && handoff_->work == motion.work;
    fact["result"] = result->payload.empty() ? Json{{"status",handing_off && !motion.action->attempted() ? "skipped" : "unconfirmed"}} :
        Json::parse(result->payload);
    const auto& receipt = fact.at("result");
    const bool departed = receipt.at("status") == "unconfirmed";
    if (handing_off && departed && !stopping && !handoff_->rechecked) {
        handoff_->rechecked = true;
        motion.action->recheck_exit();
        return;
    }
    if (departed && result->effect == GameEffect::unknown && motion.stopping && motion.skill != "follow")
        task_.hold_correction(motion.work);
    const bool paused = motion.skill == "follow" && follow_ && follow_->pausing && !stopping && !departed;
    const bool held = motion.skill == "follow" && follow_ && motion.accepted && !motion.stopping && receipt.at("status") == "blocked";
    if (motion.skill == "follow" && follow_) {
        fact["transition"] = paused ? "paused" : held ? "blocked" : "exited";
        if (paused) {
            follow_->pausing = false;
            follow_->suspended = true;
            const auto progress = receipt.value("progress",Json::object());
            const auto phase = progress.value("phase","");
            follow_->needs_route_check |= phase == "blocked" || phase == "waiting_player";
        } else if (held) {
            follow_->suspended = !follow_->needs_route_check;
            follow_->needs_route_check = true;
        }
        else follow_.reset();
    }
    publish("mc.partner.action",fact);
    if (held) track_follow(receipt,true);
    const bool verified = verified_action(receipt,motion.skill,motion.pickup);
    const bool failed = motion.queued && !motion.plan_node && !verified &&
        !(replacing_motion_ && receipt.at("status") == "interrupted");
    if (handing_off) {
        if (!stopping) task_.record(fact,false);
        handoff_->receipt = {{"status",departed ? "old_work_exit_unconfirmed" : "exited"},
            {"work",motion.work},{"action",fact}};
        publish("mc.partner.motion",handoff_->receipt);
    }
    if (motion.plan_node) {
        if (!stopping && !handing_off)
            task_.record(fact,motion.revision == task_.revision() && plan_->cancellation.empty());
        finish_plan_node(*motion.plan_node,verified,receipt);
    } else if (!stopping && !paused && !held && !handing_off) {
        task_.record(fact,motion.skill == "follow" || motion.revision == task_.revision());
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

// 原回合退出后释放本实例持权，显式接管则等游戏完成全局释放。
// 步骤：固定原释放范围 → 重投同一请求 → 发布实际结果或未获确认。
bool PartnerLoop::release_game(const BridgeConfig& config, std::uint64_t instance) {
    using namespace std::chrono;
    auto& release = *release_;
    if (release.done) return true;
    const auto now = steady_clock::now();
    if (now >= release.deadline || (release.global && release.world.empty())) {
        release.pipe.reset();
        publish("mc.partner.control",{{"generation",release.world},{"order",0},{"phase","unconfirmed"}});
        return release.done = true;
    }
    if (now < release.next) return false;
    try {
        if (release.id.empty()) {
            if (release_sequence_ == UINT64_MAX) throw std::overflow_error("MC partner: release sequence exhausted");
            release.id = "release."+std::to_string(instance)+"."+std::to_string(++release_sequence_);
            Json target{{"generation",release.world}};
            if (!release.global) target["controller"] = config.controller;
            release.arguments = target.dump();
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
    if (closing || feed_.recovering() || !request_ || !work.invocation || stopped_work_ || feed_.pending().empty() || feed_.cursor() <= reviewed_) return;
    // 与当前在途动作完全对应的唯一批准仍留在收件记录，已由确认状态机处理。
    if (confirmation_ && confirmation_->request == work.invocation && confirmation_->approved_order == feed_.cursor() &&
        feed_.pending().size() == 1 && confirmation_->approved_order > confirmation_->order) return;
    if (question_ && question_->approval && question_->item == work.invocation && question_->answer_order == feed_.cursor() &&
        feed_.pending().size() == 1) return;
    const auto input = Json{{"currentEvents",events_.empty() ? recent_events_ : events_},{"newEvents",feed_.pending()},
        {"work",{{"skill",work.skill},{"candidate",work.invocation},{"state","running"}}},
        {"player",feed_.owner()},{"generation",session_world_},{"lastObservation",feed_.observation()}}.dump();
    reviewed_ = feed_.cursor();
    const auto instructions = render_guidance("Minecraft 动作复核",review_guidance);
    const dfy_v2_agent_input assessment{view(input),view(instructions),{},{},0,0};
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
        const auto recovering = feed_.recovering();
        feed_.advance(config,instance);
        if (recovering != feed_.recovering())
            publish("mc.partner.feed",{{"generation",feed_.world().empty() ? session_world_ : feed_.world()},
                {"cursor",feed_.cursor()},{"gap",feed_.gap()},
                {"status",feed_.recovering() ? "recovering" : feed_.readable() ? "resumed" : "binding_lost"}});
        if (!feed_.control().is_null()) stop();
        if (phase_ == DFY_V2_PARTNER_LISTENING && !feed_.recovering()) {
            advance_confirmation();
            intake(work);
        }
    }
    const bool closing = phase_ == DFY_V2_PARTNER_STOPPING;
    if (closing && !release_) {
        release_.emplace();
        release_->world = feed_.world().empty() ? session_world_ : feed_.world();
        release_->deadline = std::chrono::steady_clock::now()+std::chrono::seconds(5);
        release_->global = !feed_.control().is_null();
        release_->done = !release_->global && feed_.readable() && (feed_.mode() != "partner" || feed_.execution() == "released");
        feed_.clear();
    }
    const bool departed = feed_.departure() || feed_.departed();
    const bool replaced = session_ && (feed_.world() != session_world_ || feed_.binding_revision() != session_binding_);
    const bool unavailable = closing || replaced || departed || !feed_.execution_bound();
    advance_confirmation_presentation(config,instance,unavailable);
    advance_question(config,instance,unavailable);
    if (awaiting_delegation_ && (unavailable || awaiting_delegation_->world != feed_.world())) {
        publish("mc.partner.report",{{"generation",awaiting_delegation_->world},{"id",awaiting_delegation_->id},{"ok",false},
            {"text","原游戏世界或配对玩家已离开"}});
        awaiting_delegation_.reset();
    }
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
    if (replaced || feed_.departure()) {
        if (motion_ || (plan_ && plan_->active) || review_.request || presentation_ || question_work_) return;
        const auto departure = feed_.departure();
        if (departure > feed_.cursor()) return;
        retire();
        if (departure) {
            publish("mc.partner.event",{{"generation",feed_.world()},{"event",{{"sequence",departure},{"type","player.left"}}}});
            feed_.settle_departure();
            settled_ = departure;
        }
        return;
    }
    if (unavailable || presentation_ || question_work_ || !feed_.ready() || outbound_.size() > 54) return;
    if (!feed_.pending().empty()) submit("game.event");
    else if (question_wake_) submit("question.result");
    else if (!approval_result_.is_null()) submit("approval.result");
    else if (awaiting_delegation_) submit("main.delegation");
    else if (const auto wake = autonomous_wake(); !wake.empty()) submit(wake);
}

// 借出最新角色状态，世界字符串只在本次 ABI 调用内有效。
dfy_v2_partner_status PartnerLoop::status() const noexcept {
    return {phase_,feed_.ready() && feed_.execution_ready() && !feed_.departure(),session_,feed_.cursor(),feed_.departed(),settled_,
        view(feed_.world()),view(feed_.owner())};
}

// 返回只读信箱和角色执行入口各自的游戏侧状态。
dfy_v2_partner_execution PartnerLoop::execution_status() const noexcept {
    return {feed_.readable(),0,view(feed_.mode()),view(feed_.execution())};
}
}
