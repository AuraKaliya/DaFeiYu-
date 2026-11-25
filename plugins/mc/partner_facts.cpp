#include "partner_facts.hpp"
#include <dafeiyu/tool_abi_v2.h>
#include <algorithm>
#include <limits>

namespace dfy::mc {
namespace {
using Json = nlohmann::json;
// 借用本次同步回调内仍存活的字段字节。
dfy_v2_string_view bytes(std::string_view value) { return {value.data(),static_cast<uint32_t>(value.size())}; }
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
}
// 对当前场景比较顶层字段，原始事件及工具授权随本次提交独立固定。
// 步骤：判断作用域及用途边界 → 生成变化与关闭项 → 原序号补发所需基线 → 接纳后发布新镜像。
std::uint32_t PartnerFacts::submit(std::uint64_t session, std::string_view scope, const dfy_v2_agent_input& run,
    const Json& scene, bool isolate, std::uint64_t& request) {
    if (!service_ || sequence_ == UINT64_MAX) return DFY_V2_INVALID_CONTEXT;
    bool baseline = session != session_ || scope != scope_ || isolated_ || isolate;
    const auto sequence = session == session_ ? sequence_+1 : 1;
    auto candidate = scene;
    std::string candidate_scope(scope);
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        Json changed = Json::object(), closed = Json::array();
        for (const auto& [key,value] : scene.items()) {
            if (baseline || key == "trigger" || key == "events" || !previous_.contains(key) || previous_.at(key) != value)
                changed[key] = value;
        }
        if (!baseline) for (const auto& [key,value] : previous_.items()) if (!scene.contains(key)) closed.push_back(key);
        const auto patch = Json{{"set",std::move(changed)},{"closed",std::move(closed)}}.dump();
        auto input = run; input.prompt = bytes(patch); input.flags &= ~DFY_V2_AGENT_REPLACE_HISTORY;
        const dfy_v2_fact_frame frame{session,sequence,bytes(scope),baseline ? DFY_V2_FACT_BASELINE : 0u,0};
        std::uint64_t expected = 0;
        const auto result = service_->submit(session,&input,&frame,&request,&expected,nullptr);
        if (result == DFY_V2_NOT_FOUND && expected && !baseline && attempt == 0) { baseline = true; continue; }
        if (result) return result;
        if (session_ != session || scope_ != scope) { uncertain_.clear(); finished_late_.clear(); admission_ = 0; }
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

// 只淘汰已完成查证的原工作，等待退出或等待拾取的条目继续留在原代次。
void PartnerFacts::finish_late(const std::string& work) {
    if (std::ranges::find(finished_late_,work) != finished_late_.end()) return;
    finished_late_.push_back(work);
    if (finished_late_.size() > 128) { uncertain_.erase(finished_late_.front()); finished_late_.pop_front(); }
}

// 以同一工作、版本及正文向宿主登记，调用返回前保留借用的字节。
std::uint32_t PartnerFacts::put(std::string_view work, std::uint64_t revision, std::uint32_t phase, const Json& body,
    bool final, std::uint64_t sequence) const {
    const auto text = body.dump();
    const dfy_v2_work_fact fact{session_,revision,bytes(work),bytes(text),phase,0};
    std::uint64_t receipt = 0;
    if (ordered_) return ordered_->settle_ordered(session_,&fact,sequence,final ? DFY_V2_WORK_FINAL_EFFECT : 0,&receipt,nullptr);
    return service_->settle(session_,&fact,&receipt,nullptr);
}

// 为首次派发固定原工作号、技能和原话，宿主写入成功后才消耗接纳序号。
std::uint32_t PartnerFacts::reserve(std::string_view work, std::string_view skill, std::uint64_t order) {
    if (admission_ == UINT64_MAX) return DFY_V2_RESOURCE_EXHAUSTED;
    const auto status = put(work,1,DFY_V2_WORK_ACCEPTED,{{"skill",skill},{"sourceMessage",order}},false,admission_+1);
    if (!status) ++admission_;
    return status;
}

// 桥接只为已退出的原投物工作提供复查，拾取数量由独立的服务端事件确认。
// 步骤：按原号登记退出 → 核对接收人与完整数量 → 登记晚到实收并保留重复查询的原版本。
std::uint32_t PartnerFacts::report_handoff(const Json& handoff) {
    const auto& work = handoff.at("work").get_ref<const std::string&>();
    const auto late = uncertain_.find(work);
    if (late != uncertain_.end() && !late->second.exited) {
        if (const auto result = put(work,3,DFY_V2_WORK_EXIT,{{"exited",true}})) return result;
        late->second.exited = true;
    }

    const auto& proof = handoff.at("receipt");
    if (proof.at("source") != "server_events" || proof.at("recipientPicked") != handoff.at("requested") ||
        proof.at("throwUnknown") != 0 || proof.at("pickupUnknown") != 0 || handoff.at("originalStatus") != "blocked") return DFY_V2_OK;
    const auto revision = late != uncertain_.end() && late->second.final_effect ? 5u : 4u;
    const auto result = put(work,revision,DFY_V2_WORK_EFFECT,{{"recipient",handoff.at("recipient")},{"item",handoff.at("item")},
        {"recipientPicked",proof.at("recipientPicked")},{"stream",proof.at("stream")},{"sourceThrough",handoff.at("sourceThrough")}},true);
    if (!result && late != uncertain_.end()) { late->second.picked = true; finish_late(work); }
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
    const auto late = uncertain_.find(work);
    if (!exited || late == uncertain_.end()) {
        if (!exited) effect["bridgeExited"] = false;
        result = put(work,2,DFY_V2_WORK_EFFECT,effect,complete);
        if (result) return result;
        if (!exited) {
            uncertain_.try_emplace(work);
            return DFY_V2_OK;
        }
    }
    result = put(work,3,DFY_V2_WORK_EXIT,{{"exited",true}});
    if (result) return result;
    if (late != uncertain_.end()) {
        late->second.exited = true;
        // 已送达的拾取比原动作结束时的库存更新，迟到读回不能覆盖更晚的实收。
        if (!late->second.picked) {
            result = put(work,4,DFY_V2_WORK_EFFECT,effect,complete);
            if (result) return result;
            late->second.final_effect = true;
            if (complete) finish_late(work);
        }
    }
    return handoff_status;
}
}
