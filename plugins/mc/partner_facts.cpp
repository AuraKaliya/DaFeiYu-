#include "partner_facts.hpp"
#include <limits>

namespace dfy::mc {
namespace {
using Json = nlohmann::json;
// 借用本次同步回调内仍存活的字段字节。
dfy_v2_string_view bytes(std::string_view value) { return {value.data(),static_cast<uint32_t>(value.size())}; }
}
// 固定协商成功的可选接口及玩家身份，新的控制周期重新建立基线。
void PartnerFacts::configure(const dfy_v2_agent_facts_api* service, std::string player) {
    clear(); service_ = service; player_ = std::move(player);
}
// 退还旧作用域镜像，保留所配置的唯一玩家。
void PartnerFacts::clear() noexcept {
    scope_.clear(); previous_ = nullptr; sequence_ = session_ = 0; isolated_ = false;
    uncertain_.clear();
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
        if (session_ != session || scope_ != scope) uncertain_.clear();
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

// 以同一工作、版本及正文向宿主登记，调用返回前保留借用的字节。
std::uint32_t PartnerFacts::put(std::string_view work, std::uint64_t revision, std::uint32_t phase, const Json& body) const {
    const auto text = body.dump();
    const dfy_v2_work_fact fact{session_,revision,bytes(work),bytes(text),phase,0};
    std::uint64_t receipt = 0;
    return service_->settle(session_,&fact,&receipt,nullptr);
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
        {"recipientPicked",proof.at("recipientPicked")},{"stream",proof.at("stream")},{"sourceThrough",handoff.at("sourceThrough")}});
    if (!result && late != uncertain_.end()) late->second.picked = true;
    return result;
}

// 以桥接回执区分工具实效和游戏退出，同号复查可补记迟到的终态。
// 步骤：绑定原动作 → 登记首次实效 → 依据已校验退出补齐原工作 → 按观察回查原交付。
std::uint32_t PartnerFacts::report(std::uint64_t session, std::string_view category, const Json& payload) {
    if (!service_ || !session || session != session_) return DFY_V2_OK;
    const bool accepted = category == "mc.partner.motion" && payload.value("status","") == "accepted";
    if (!accepted && category != "mc.partner.action") return DFY_V2_OK;
    const auto& action = accepted ? payload.at("action") : payload;
    const auto work = action.value("bridgeWork",std::string{});
    if (work.empty() || action.value("session",std::uint64_t{}) != session ||
        (!accepted && !action.contains("bridgeExited"))) return DFY_V2_OK;
    const Json source{{"skill",action.at("skill")},{"sourceMessage",action.at("order")}};
    auto result = put(work,1,DFY_V2_WORK_ACCEPTED,source);
    if (accepted || result) return result;

    const bool exited = action.at("bridgeExited").get<bool>();
    Json effect{{"effect",action.at("effect")}};
    if (action.contains("result") && action.at("result").dump().size() <= 7000) effect["result"] = action.at("result");
    else effect["originalWork"] = work;
    const auto late = uncertain_.find(work);
    if (!exited || late == uncertain_.end()) {
        if (!exited) effect["bridgeExited"] = false;
        result = put(work,2,DFY_V2_WORK_EFFECT,effect);
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
            result = put(work,4,DFY_V2_WORK_EFFECT,effect);
            if (result) return result;
            late->second.final_effect = true;
        }
    }
    if (action.at("skill") == "observe" && action.contains("result") && action.at("result").contains("handoffReceipt"))
        return report_handoff(action.at("result").at("handoffReceipt"));
    return DFY_V2_OK;
}
}
