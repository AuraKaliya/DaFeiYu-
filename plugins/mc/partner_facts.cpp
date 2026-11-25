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
        previous_ = std::move(candidate); scope_ = std::move(candidate_scope);
        sequence_ = sequence; session_ = session; isolated_ = isolate;
        return DFY_V2_OK;
    }
    return DFY_V2_FAILED;
}
// 按原桥接工作记接纳、实效和退出，主动取证与游戏执行使用不同接口。
// 步骤：定位当前作用域的原工作 → 在同号登记来源 → 保存实际效果与退出回执。
std::uint32_t PartnerFacts::report(std::uint64_t session, std::string_view category, const Json& payload) const {
    if (!service_ || !session || session != session_) return DFY_V2_OK;
    const bool accepted = category == "mc.partner.motion" && payload.value("status","") == "accepted";
    if (!accepted && category != "mc.partner.action") return DFY_V2_OK;
    const auto& action = accepted ? payload.at("action") : payload;
    const auto work = action.value("bridgeWork",std::string{});
    if (work.empty() || action.value("session",std::uint64_t{}) != session) return DFY_V2_OK;
    const auto put = [&](uint64_t revision, uint32_t phase, const Json& body) {
        const auto text = body.dump();
        const dfy_v2_work_fact fact{session,revision,bytes(work),bytes(text),phase,0};
        std::uint64_t receipt = 0;
        return service_->settle(session,&fact,&receipt,nullptr);
    };
    const Json source{{"skill",action.at("skill")},{"sourceMessage",action.at("order")}};
    auto result = put(1,DFY_V2_WORK_ACCEPTED,source);
    if (accepted || result) return result;
    Json effect{{"effect",action.at("effect")}};
    if (action.contains("result") && action.at("result").dump().size() <= 7000) effect["result"] = action.at("result");
    else effect["originalWork"] = work;
    result = put(2,DFY_V2_WORK_EFFECT,effect);
    return result ? result : put(3,DFY_V2_WORK_EXIT,{{"exited",true}});
}
}
