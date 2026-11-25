#include "binding_facts.hpp"
#include "../plugins/mc/partner_facts.hpp"
#include <dafeiyu/tool_abi_v2.h>
#include <algorithm>
#include <iostream>

namespace {
using Json = nlohmann::json;
dfy::app::BindingWorks* ledger = nullptr;

// 在固定子会话中接纳一份基线，所有工作回执仍使用真实宿主索引。
dfy_v2_status DFY_V2_CALL submit(uint64_t, const dfy_v2_agent_input*, const dfy_v2_fact_frame*,
    uint64_t* request, uint64_t*, dfy_v2_error*) { *request = 1; return DFY_V2_OK; }

// 将 C 借用值转交给真实宿主索引，按同一 ABI 区分容量与旧号错误。
dfy_v2_status DFY_V2_CALL record(uint64_t session, const dfy_v2_work_fact* fact, uint64_t sequence,
    uint32_t flags, uint64_t* receipt, dfy_v2_error*) {
    try {
        *receipt = ledger->settle(session,{fact->epoch,fact->revision,{fact->work.data,fact->work.length},
            {fact->content.data,fact->content.length},fact->phase,sequence,(flags & DFY_V2_WORK_FINAL_EFFECT)!=0});
        return DFY_V2_OK;
    } catch (const std::length_error&) { return DFY_V2_RESOURCE_EXHAUSTED; }
    catch (const std::invalid_argument&) { return DFY_V2_INVALID_ARGUMENT; }
}
// 第一版夹具沿无序接纳路径回报。
dfy_v2_status DFY_V2_CALL legacy(uint64_t session, const dfy_v2_work_fact* fact, uint64_t* receipt, dfy_v2_error* error) {
    return record(session,fact,0,0,receipt,error);
}

// 比较试点的新旧事实服务在满额读回时对同一投物与实收的登记。
// 步骤：登记已投出工作 → 填满活动槽 → 沿观察回执更新原号 → 核对重传和可回收的槽。
void late_pickup(bool ordered, bool recyclable) {
    dfy::app::BindingWorks works; ledger = &works;
    dfy_v2_agent_ordered_facts_api api{{{},submit,legacy},record,nullptr};
    dfy::mc::PartnerFacts facts;
    facts.configure(&api.base,"paired",ordered ? &api : nullptr);
    std::uint64_t request = 0;
    if (facts.submit(1,"paired/world",{},Json::object(),false,request) || facts.reserve("give","give_item",1))
        throw std::runtime_error("original work not admitted");
    Json original{{"session",1},{"order",1},{"skill","give_item"},{"bridgeWork","give"},{"bridgeExited",true},{"factAdmission",0},
        {"effect",DFY_V2_TOOL_APPLIED},{"result",{{"status","blocked"},{"receipt",{{"source","server_events"},{"throwUnknown",0},{"pickupUnknown",8}}}}}};
    if (facts.report(1,"mc.partner.action",original)) throw std::runtime_error("missing original pending receipt");
    for (unsigned n=1;n<128;++n) {
        const auto work = "other-"+std::to_string(n);
        if (facts.reserve(work,"observe",1)) throw std::runtime_error("fixture could not fill the work index");
        if (recyclable && facts.report(1,"mc.partner.action",{{"session",1},{"order",1},{"skill","observe"},{"bridgeWork",work},
            {"bridgeExited",true},{"factAdmission",0},{"effect",DFY_V2_TOOL_NOT_APPLIED},{"result",{{"status","done"}}}}))
            throw std::runtime_error("completed read did not settle");
    }
    const auto status = facts.reserve("later-read","observe",2);
    if (status != (recyclable ? DFY_V2_OK : DFY_V2_RESOURCE_EXHAUSTED)) throw std::runtime_error("wrong preflight result");
    const Json handoff{{"work","give"},{"recipient","paired"},{"item","cobblestone"},{"requested",8},{"sourceThrough",12},
        {"originalStatus","blocked"},{"receipt",{{"source","server_events"},{"recipientPicked",8},{"stream","s"},{"throwUnknown",0},{"pickupUnknown",0}}}};
    const Json observed{{"session",1},{"order",2},{"skill","observe"},{"bridgeWork","later-read"},{"bridgeExited",true},
        {"factAdmission",status},{"effect",DFY_V2_TOOL_NOT_APPLIED},{"result",{{"handoffReceipt",handoff}}}};
    if (facts.report(1,"mc.partner.action",observed)!=status) throw std::runtime_error("reader dropped its own recording status");
    const auto watermark = works.watermark();
    if (facts.report(1,"mc.partner.action",observed)!=status || watermark!=works.watermark()) throw std::runtime_error("pickup replay advanced ledger");
    const auto results = works.view(1,0);
    const auto item = std::ranges::find_if(results,[](const Json& row) { return row.at("work")=="give"; });
    if (item==results.end() || item->at("accepted")!=1 || item->at("revision")!=4 || item->at("effect").at("recipientPicked")!=8 ||
        item->at("effectReceipt")<=item->at("exited") || item->value("finalEffect",false)!=ordered)
        throw std::runtime_error("full index lost original pickup, exit or finality");
    if (ordered && facts.reserve("next","reply",3)) throw std::runtime_error("final pickup did not release an eligible slot");
}
}

// 验证所有未决和存在可回收记录两种满额状态，覆盖旧事实接口。
int main() {
    try { late_pickup(false,false); late_pickup(true,false); late_pickup(true,true); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
