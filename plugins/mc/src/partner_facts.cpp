#include "partner_facts.hpp"
#include <dafeiyu/tool_abi_v2.h>
#include <algorithm>
#include <limits>

namespace dfy::mc {
namespace {
using Json = nlohmann::json;
// 借用本次同步回调内仍存活的字段字节。
dfy_v2_string_view bytes(std::string_view value) { return {value.data(),static_cast<uint32_t>(value.size())}; }

// 从配对玩家的已接纳任务提取原执行修订和目标，缺少来源时保留缺样。
Json task_goal_sample(const Json& scene, const std::string& player) {
    const auto task = scene.find("task"), subject = scene.find("player");
    if (task == scene.end() || player.empty() || subject == scene.end() || *subject != player) return nullptr;
    const auto revision = task->find("revision"), goal = task->find("goal");
    if (revision == task->end() || !revision->is_number_unsigned() || *revision == 0 ||
        goal == task->end() || !goal->is_string() || goal->get_ref<const std::string&>().empty()) return nullptr;
    return {{"key","mc.companion.task_goal"},{"version",1},{"subject",player},{"fact",*revision},{"value",{{"goal",*goal}}}};
}

// 从等待中的原工作和已入信箱的终态生成关注，原始操作参数仍随领域候选提供。
// 步骤：提取可见任务目标 → 登记原候选的任务期 → 按原消息号提供计次采样。
void decision_facts(Json& scene, const std::string& player, const std::deque<Json>& approvals, bool same_scope, bool relevant) {
    Json samples = Json::array(), attention = Json::array();
    if (auto goal = task_goal_sample(scene,player); !goal.is_null()) samples.push_back(std::move(goal));
    if (relevant && scene.contains("player") && scene.at("player") == player) {
        const auto pending = scene.find("pendingApproval");
        if (pending != scene.end() && pending->value("status","") == "waiting" && pending->contains("work")) {
            const auto& work = pending->at("work");
            samples.push_back({{"key","mc.companion.pending_approval"},{"version",1},{"subject",work},{"fact",pending->at("item")},
                {"value",{{"order",pending->at("order")},{"skill",pending->at("skill")}}}});
            attention.push_back({{"key","mc.companion.pending_approval"},{"version",1},{"subject",work},{"mode","continuous"},{"task",work}});
        }
        if (same_scope) for (const auto& receipt : approvals) {
            const auto& work = receipt.at("work");
            const auto& message = receipt.at("message");
            samples.push_back({{"key","mc.companion.approval_result"},{"version",1},{"subject",work},{"fact",message},{"value",receipt.at("value")}});
            attention.push_back({{"key","mc.companion.approval_result"},{"version",1},{"subject",work},{"fact",message},
                {"mode","counted"},{"count",1},{"event",message}});
        }
    }
    if (!samples.empty()) scene["decisionStatusSamples"] = std::move(samples);
    if (!attention.empty()) scene["decisionStatusAttention"] = std::move(attention);
}

// 核对小型拾取摘要的必填类型、时间和余量口径，旧桥接及不完整回执继续保留原文。
// 步骤：核对来源与计数范围 → 对照已入包及他人收取 → 区分现存、核清和未知。
bool verified_pickup(const Json& value) {
    if (!value.is_object() || value.size()!=(value.contains("pendingPickup") ? 10u : 9u) ||
        !value.contains("sampledUtcMs") || !value.at("sampledUtcMs").is_number_integer()) return false;
    for (const auto name : {"entity","observation","dimension","item","remainingState"})
        if (!value.contains(name) || !value.at(name).is_string() || value.at(name).get_ref<const std::string&>().empty()) return false;
    for (const auto name : {"target","acquired","other"})
        if (!value.contains(name) || !value.at(name).is_number_integer() || value.at(name)<0 || value.at(name)>INT32_MAX) return false;
    const auto total = value.at("target").get<std::int64_t>();
    const auto received = value.at("acquired").get<std::int64_t>()+value.at("other").get<std::int64_t>();
    if (total<1 || received>total) return false;
    const auto pending = value.find("pendingPickup");
    if (value.at("remainingState")=="unknown") return pending==value.end();
    if (pending==value.end() || !pending->is_number_integer() || *pending<0 || *pending>total-received) return false;
    return value.at("remainingState")=="observed" ? *pending>0 : value.at("remainingState")=="accounted" && *pending==0;
}
// 保留原箱内转移工作的方向、对象、委托、提交范围和实读基线，供迟到回执逐项对照。
Json chest_transfer_basis(Json proof) {
    for (const auto field : {"state","after","stored","withdrawn","remaining"}) proof.erase(field);
    return proof;
}
// 判定桥接退出后是否还有待查实效，给物须以服务器记录核清投出和去向。
bool final_effect(const Json& action) {
    const auto effect = action.at("effect").get<std::uint32_t>();
    if (effect == DFY_V2_TOOL_UNKNOWN) return false;
    if (action.at("skill") != "give_item" || effect == DFY_V2_TOOL_NOT_APPLIED) return true;
    if (!action.contains("result") || !action.at("result").contains("receipt")) return false;
    const auto& proof = action.at("result").at("receipt");
    return proof.value("source","") == "server_events" && proof.value("throwUnknown",-1)==0 && proof.value("pickupUnknown",-1)==0;
}
}
// 固定协商成功的可选接口及玩家身份，新的控制周期重新建立基线。
void PartnerFacts::configure(const dfy_v2_agent_facts_api* service, std::string player, const dfy_v2_agent_ordered_facts_api* ordered) {
    clear(); service_ = service; ordered_ = ordered; player_ = std::move(player);
}
// 只为选定玩家协商事实能力，两版使用相同的回合提交入口。
// 步骤：核对试点配置 → 按可用版本校验服务表 → 绑定同一玩家和原件接纳路径。
std::uint32_t PartnerFacts::connect(dfy_v2_query_interface_fn host, const std::string& player) {
    configure(nullptr,player);
    if (player.empty()) return DFY_V2_OK;
    for (auto version : {DFY_V2_AGENT_ORDERED_FACTS_VERSION,DFY_V2_AGENT_FACTS_VERSION}) {
        const void* table = nullptr;
        const auto status = host(bytes(DFY_V2_AGENT_FACTS_INTERFACE),version,&table,nullptr);
        if (status == DFY_V2_UNSUPPORTED) continue;
        if (status) return status;
        const auto* base = static_cast<const dfy_v2_agent_facts_api*>(table);
        const auto* ordered = version==DFY_V2_AGENT_ORDERED_FACTS_VERSION ? static_cast<const dfy_v2_agent_ordered_facts_api*>(table) : nullptr;
        if (!base || base->header.struct_size!=(ordered ? sizeof(*ordered) : sizeof(*base)) || base->header.interface_version!=version ||
            !base->submit || !base->settle || (ordered && (!ordered->settle_ordered || !ordered->read_receipt))) return DFY_V2_ABI_MISMATCH;
        configure(base,player,ordered);
        break;
    }
    return DFY_V2_OK;
}
// 退还旧作用域镜像，保留所配置的唯一玩家。
void PartnerFacts::clear() noexcept {
    scope_.clear(); previous_ = nullptr; sequence_ = session_ = admission_ = 0; isolated_ = false;
    uncertain_.clear();
    finished_late_.clear();
    approvals_.clear();
    pickup_sample_ = nullptr; pickup_task_.reset();
}
// 从信箱接纳回执固定原工作、裁定和执行结果，按到达顺序保留最近十六份供下一相关决策使用。
// 步骤：核对来源绑定 → 沿原消息识别重投 → 保留有界可重读原件。
bool PartnerFacts::accept_approval(std::uint64_t session, std::uint64_t mailbox, std::uint64_t message, const Json& result) {
    if (!bound(session) || !mailbox || !message || result.at("player") != player_) return false;
    if (!approvals_.empty() && approvals_.back().at("message") >= message) return false;
    Json value{{"mailbox",mailbox}};
    for (auto field : {"id","order","skill","status","source","execution","player","generation"}) value[field] = result.at(field);
    approvals_.push_back({{"work",result.at("work")},{"message",message},{"value",std::move(value)}});
    if (approvals_.size()>16) approvals_.pop_front();
    return true;
}
// 把原绑定及未决工作交给迟到回执，活动场景和新的接纳序号留在各自会话。
// 步骤：建立只含原会话地址的结算对象 → 移交原工作状态 → 交回共同所有权。
std::shared_ptr<PartnerFacts> PartnerFacts::retire_settlements() {
    auto retired = std::make_shared<PartnerFacts>();
    retired->service_ = service_; retired->ordered_ = ordered_; retired->session_ = session_;
    retired->uncertain_.swap(uncertain_);
    retired->finished_late_.swap(finished_late_);
    return retired;
}
// 以本绑定已接纳的完整场景和目标采样为基准，来源消失时在原帧关闭旧样本。
// 步骤：生成本次场景与采样 → 比较变化和关闭项 → 原序号补发所需基线 → 接纳后发布新镜像。
std::uint32_t PartnerFacts::submit(std::uint64_t session, std::string_view scope, const dfy_v2_agent_input& run,
    const Json& scene, bool isolate, std::uint64_t& request) {
    if (!service_ || sequence_ == UINT64_MAX) return DFY_V2_INVALID_CONTEXT;
    bool baseline = session != session_ || scope != scope_ || isolated_ || isolate;
    const auto sequence = session == session_ ? sequence_+1 : 1;
    auto candidate = scene;
    const bool same_scope = session == session_ && scope == scope_;
    decision_facts(candidate,player_,approvals_,same_scope,!isolate);
    if (same_scope && !isolate && !pickup_sample_.is_null() && scene.contains("player") && scene.at("player") == player_) {
        candidate["decisionStatusSamples"].push_back(pickup_sample_);
        const auto task = scene.find("task");
        if (pickup_task_ && task != scene.end() && task->contains("revision") && task->at("revision") == *pickup_task_ &&
            task->value("next","")!="done") {
            const auto& work = pickup_sample_.at("subject");
            candidate["decisionStatusAttention"].push_back({{"key","mc.companion.pickup_effect"},{"version",1},{"subject",work},
                {"mode","continuous"},{"task","task/"+std::to_string(*pickup_task_)}});
        }
    }
    std::string candidate_scope(scope);
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        Json changed = Json::object(), closed = Json::array();
        for (const auto& [key,value] : candidate.items()) {
            if (baseline || key == "trigger" || key == "events" || !previous_.contains(key) || previous_.at(key) != value)
                changed[key] = value;
        }
        if (!baseline) for (const auto& [key,value] : previous_.items()) if (!candidate.contains(key)) closed.push_back(key);
        const auto patch = Json{{"set",std::move(changed)},{"closed",std::move(closed)}}.dump();
        auto input = run; input.prompt = bytes(patch); input.flags &= ~DFY_V2_AGENT_REPLACE_HISTORY;
        const dfy_v2_fact_frame frame{session,sequence,bytes(scope),baseline ? DFY_V2_FACT_BASELINE : 0u,0};
        std::uint64_t expected = 0;
        const auto result = service_->submit(session,&input,&frame,&request,&expected,nullptr);
        if (result == DFY_V2_NOT_FOUND && expected && !baseline && attempt == 0) { baseline = true; continue; }
        if (result) return result;
        if (!same_scope) {
            uncertain_.clear(); finished_late_.clear(); approvals_.clear(); admission_ = 0;
            pickup_sample_ = nullptr; pickup_task_.reset();
        }
        previous_ = std::move(candidate); scope_ = std::move(candidate_scope);
        sequence_ = sequence; session_ = session; isolated_ = isolate;
        return DFY_V2_OK;
    }
    return DFY_V2_FAILED;
}
// 只在本代次成功登记过退出未知时保留原工作的查证责任。
bool PartnerFacts::needs_exit(std::uint64_t session, const std::string& work) const noexcept {
    const auto found = uncertain_.find(work);
    return session == session_ && found != uncertain_.end() && !found->second.exited;
}

// 只保留已退出且有原投物可追踪的未决收件。
bool PartnerFacts::needs_pickup(std::uint64_t session, const std::string& work) const noexcept {
    const auto found = uncertain_.find(work);
    return bound(session) && found != uncertain_.end() && found->second.pending_pickup;
}

// 沿投影给出的回执号读取本绑定的原件或归档指纹，独立于游戏执行通道。
// 步骤：核对当前代次和非零回执号 → 借用有界缓冲读回 → 返回实际命中或不可用状态。
Json PartnerFacts::read_work_receipt(std::uint64_t session, const Json& args) const {
    if (args.size()!=3 || args.at("mode")!="work_receipt" || !args.at("epoch").is_number_unsigned() ||
        !args.at("receipt").is_number_unsigned() || args.at("receipt")==0) throw std::invalid_argument("MC: invalid work receipt reference");
    if (!can_read_receipts() || !bound(session) || args.at("epoch")!=session)
        return {{"status","unavailable"},{"reasonCode","work_receipt_scope"}};

    std::string buffer(16*1024,'\0');
    std::uint32_t size = 0;
    const auto receipt = args.at("receipt").get<std::uint64_t>();
    const auto status = ordered_->read_receipt(session,session,receipt,buffer.data(),static_cast<std::uint32_t>(buffer.size()),&size,nullptr);
    if (status || !size || size>buffer.size()) return {{"status","unavailable"},{"reasonCode","work_receipt_unavailable"},{"factStatus",status}};
    buffer.resize(size);
    return {{"status","done"},{"workReceipt",Json::parse(buffer)}};
}

// 确认查证回到同一工作和原绑定，再追加已经核实的去向。
std::uint32_t PartnerFacts::settle_pickup(std::uint64_t session, const std::string& work, const Json& sample) {
    const auto original = uncertain_.find(work);
    if (!bound(session) || sample.at("work") != work || original == uncertain_.end() || !original->second.handoff)
        return DFY_V2_INVALID_CONTEXT;
    return report_handoff(sample);
}

// 只淘汰已完成查证的原工作，等待退出或等待拾取的条目继续留在原代次。
void PartnerFacts::finish_late(const std::string& work) {
    if (std::ranges::find(finished_late_,work) != finished_late_.end()) return;
    finished_late_.push_back(work);
    if (finished_late_.size() > 128) { uncertain_.erase(finished_late_.front()); finished_late_.pop_front(); }
}

// 以同一工作、版本及正文向宿主登记，调用返回前保留借用的字节。
std::uint32_t PartnerFacts::put(std::string_view work, std::uint64_t revision, std::uint32_t phase, const Json& body,
    bool final, std::uint64_t sequence, std::uint64_t* recorded) const {
    const auto text = body.dump();
    const dfy_v2_work_fact fact{session_,revision,bytes(work),bytes(text),phase,0};
    std::uint64_t receipt = 0;
    const auto status = ordered_ ? ordered_->settle_ordered(session_,&fact,sequence,final ? DFY_V2_WORK_FINAL_EFFECT : 0,&receipt,nullptr) :
        service_->settle(session_,&fact,&receipt,nullptr);
    if (!status && recorded) *recorded = receipt;
    return status;
}

// 为首次派发固定原工作号、技能和原话，宿主写入成功后才消耗接纳序号。
std::uint32_t PartnerFacts::reserve(std::string_view work, std::string_view skill, std::uint64_t order) {
    if (admission_ == UINT64_MAX) return DFY_V2_RESOURCE_EXHAUSTED;
    const auto status = put(work,1,DFY_V2_WORK_ACCEPTED,{{"skill",skill},{"sourceMessage",order}},false,admission_+1);
    if (!status) ++admission_;
    return status;
}

// 桥接只为已退出的原投物工作提供复查，拾取数量由独立的服务端事件确认。
// 步骤：按原号登记退出 → 核对已提交物品的全部去向 → 登记晚到实收并保留重复查询的原版本。
std::uint32_t PartnerFacts::report_handoff(const Json& handoff) {
    const auto& work = handoff.at("work").get_ref<const std::string&>();
    const auto late = uncertain_.find(work);
    if (late == uncertain_.end() || !late->second.handoff) return DFY_V2_OK;
    if (late->second.withdrawal_work != handoff.value("withdrawalWork","")) return DFY_V2_INVALID_ARGUMENT;
    if (!late->second.exited) {
        if (const auto result = put(work,3,DFY_V2_WORK_EXIT,{{"exited",true}})) return result;
        late->second.exited = true;
    }

    const auto& proof = handoff.at("receipt");
    if (proof.at("source") != "server_events" || proof.at("throwUnknown") != 0 || proof.at("pickupUnknown") != 0) return DFY_V2_OK;
    const auto revision = late->second.effect_after_exit ? 5u : 4u;
    Json effect{{"recipient",handoff.at("recipient")},{"item",handoff.at("item")},
        {"requested",handoff.at("requested")},{"submitted",handoff.at("submitted")},{"otherPicked",proof.at("otherPicked")},
        {"recipientPicked",proof.at("recipientPicked")},{"stream",proof.at("stream")},{"sourceThrough",handoff.at("sourceThrough")}};
    if (handoff.contains("withdrawalWork")) effect["withdrawalWork"] = handoff.at("withdrawalWork");
    const auto result = put(work,revision,DFY_V2_WORK_EFFECT,effect,true);
    if (!result) {
        late->second.pickup_settled = true; late->second.pending_pickup = false;
        finish_late(work);
    }
    return result;
}

// 以桥接回执区分工具实效和游戏退出，同号复查可补记迟到的终态。
// 步骤：绑定原动作 → 登记首次实效 → 依据已校验退出补齐原工作 → 按观察回查原交付。
std::uint32_t PartnerFacts::report(std::uint64_t session, std::string_view category, const Json& payload) {
    if (!bound(session)) return DFY_V2_OK;
    const bool accepted = category == "mc.partner.motion" && payload.value("status","") == "accepted";
    if (!accepted && category != "mc.partner.action") return DFY_V2_OK;
    const auto& action = accepted ? payload.at("action") : payload;
    const auto work = action.value("bridgeWork",std::string{});
    if (work.empty() || action.value("session",std::uint64_t{}) != session) return DFY_V2_OK;
    const bool unsent = action.contains("bridgeSubmitted") && !action.at("bridgeSubmitted").get<bool>();
    if (!accepted && !unsent && !action.contains("bridgeExited")) return DFY_V2_OK;

    std::uint32_t handoff_status = DFY_V2_OK;
    if (!accepted && action.at("skill") == "observe" && action.contains("result") && action.at("result").contains("handoffReceipt"))
        handoff_status = report_handoff(action.at("result").at("handoffReceipt"));
    if (!accepted && action.at("skill") == "observe" && action.contains("result") && action.at("result").contains("chestReceipt")) {
        const auto& proof = action.at("result").at("chestReceipt");
        const auto& original = proof.at("work").get_ref<const std::string&>();
        const auto late = uncertain_.find(original);
        if (late != uncertain_.end() && !late->second.chest_basis.is_null() && !late->second.chest_settled && proof.at("state") == "verified") {
            if (chest_transfer_basis(proof) != late->second.chest_basis) return DFY_V2_INVALID_ARGUMENT;
            const auto revision = late->second.effect_after_exit ? 5u : 4u;
            Json effect{{"item",proof.at("item")},{"cell",proof.at("cell")},{"remaining",proof.at("remaining")},{"sourceMessage",proof.at("sourceMessage")}};
            if (proof.contains("direction")) {
                for (const auto field : {"direction","destination","withdrawn"}) effect[field] = proof.at(field);
                if (proof.contains("deliverTo")) effect["deliverTo"] = proof.at("deliverTo");
            } else effect["stored"] = proof.at("stored");
            handoff_status = put(original,revision,DFY_V2_WORK_EFFECT,effect,true);
            if (!handoff_status) { late->second.chest_settled = true; finish_late(original); }
        }
    }
    auto result = action.contains("factAdmission") ? action.at("factAdmission").get<std::uint32_t>() :
        put(work,1,DFY_V2_WORK_ACCEPTED,{{"skill",action.at("skill")},{"sourceMessage",action.at("order")}});
    if (accepted || result) return handoff_status ? handoff_status : result;
    if (unsent) {
        result = put(work,2,DFY_V2_WORK_EFFECT,{{"effect",DFY_V2_TOOL_NOT_APPLIED},{"dispatch","not_started"}},true);
        return result ? result : put(work,3,DFY_V2_WORK_EXIT,{{"dispatch","not_started"}});
    }

    const bool exited = action.at("bridgeExited").get<bool>();
    const bool complete = exited && final_effect(action);
    Json effect{{"effect",action.at("effect")}};
    if (action.contains("result") && action.at("result").dump().size() <= 7000) effect["result"] = action.at("result");
    else effect["originalWork"] = work;
    const auto original = action.find("result");
    const Json* pickup = nullptr;
    if (exited && action.at("skill") == "pickup_drop" && original != action.end() && original->contains("pickup")) {
        const auto& proof = original->at("pickup");
        if (proof.contains("effect") && proof.at("effect").dump().size() <= 2048 && verified_pickup(proof.at("effect")))
            pickup = &proof.at("effect");
    }
    if (pickup && !effect.contains("result")) effect["pickupEffect"] = *pickup;
    // 只投影已经写入原工作账本的那一版实效，退出补证和重传沿用原回执号。
    // 步骤：登记原工作实效 → 核对成功回执及采样 → 留下最新一份的原号。
    const auto put_effect = [&](std::uint64_t revision) {
        std::uint64_t receipt = 0;
        const auto status = put(work,revision,DFY_V2_WORK_EFFECT,effect,complete,0,&receipt);
        if (!status && receipt && pickup && (pickup_sample_.is_null() || pickup_sample_.at("fact") < receipt)) {
            auto value = *pickup;
            const auto sampled = value.at("sampledUtcMs"); value.erase("sampledUtcMs");
            pickup_sample_ = {{"key","mc.companion.pickup_effect"},{"version",1},{"subject",work},{"fact",receipt},
                {"sampledUtcMs",sampled},{"value",std::move(value)}};
            const auto task = action.value("taskRevision",std::uint64_t{});
            pickup_task_ = task ? std::optional(task) : std::nullopt;
        }
        return status;
    };
    const auto late = uncertain_.find(work);
    const bool exit_was_unknown = late != uncertain_.end() && late->second.exit_was_unknown;
    if (!exited || !exit_was_unknown) {
        if (!exited) effect["bridgeExited"] = false;
        result = put_effect(2);
        if (result) return result;
        if (!exited) {
            auto& state = uncertain_[work];
            state.exit_was_unknown = true; state.handoff = action.at("skill") == "give_item";
            if (state.handoff) state.withdrawal_work = action.value("withdrawalWork","");
            return DFY_V2_OK;
        }
    }
    result = put(work,3,DFY_V2_WORK_EXIT,{{"exited",true}});
    if (result) return result;
    if (exit_was_unknown) {
        late->second.exited = true;
        // 已送达的拾取比原动作结束时的库存更新，迟到读回不能覆盖更晚的实收。
        if (!late->second.pickup_settled) {
            result = put_effect(4);
            if (result) return result;
            late->second.effect_after_exit = true;
            if (complete) finish_late(work);
        }
    }
    if (!complete && action.at("skill") == "give_item" && action.contains("result") && action.at("result").contains("receipt")) {
        const auto& proof = action.at("result").at("receipt");
        auto& state = uncertain_[work];
        state.handoff = true; state.exited = true;
        if (action.at("result").contains("transfer")) state.withdrawal_work = action.at("result").at("transfer").value("withdrawalWork","");
        state.pending_pickup = !state.pickup_settled && proof.value("source","") == "server_events" && proof.value("pickupUnknown",0) > 0;
    }
    if (!complete && exited && (action.at("skill") == "store_item" || action.at("skill") == "withdraw_item") && action.contains("result")) {
        const auto field = action.at("skill") == "withdraw_item" ? "withdrawal" : "deposit";
        if (action.at("result").contains(field) && action.at("result").at(field).at("state") == "pending")
            uncertain_[work].chest_basis = chest_transfer_basis(action.at("result").at(field));
    }
    return handoff_status;
}
}
