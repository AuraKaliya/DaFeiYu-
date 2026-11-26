#include "binding_facts.hpp"
#include "../../plugins/mc/src/partner_facts.hpp"
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
    const Json handoff{{"work","give"},{"recipient","paired"},{"item","cobblestone"},{"requested",8},{"submitted",8},{"sourceThrough",12},
        {"originalStatus","blocked"},{"receipt",{{"source","server_events"},{"recipientPicked",8},{"otherPicked",0},{"stream","s"},{"throwUnknown",0},{"pickupUnknown",0}}}};
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

// 核对迟到存箱实效只更新同一格位、同一委托和同一基线的原工作。
// 步骤：登记中断和实际退出 → 拒绝换箱及换基线 → 接受原件并验证重传防重。
void late_deposit() {
    dfy::app::BindingWorks works; ledger = &works;
    dfy_v2_agent_ordered_facts_api api{{{},submit,legacy},record,nullptr};
    dfy::mc::PartnerFacts facts;
    facts.configure(&api.base,"paired",&api);
    std::uint64_t request = 0;
    if (facts.submit(1,"paired/world",{},Json::object(),false,request) || facts.reserve("deposit","store_item",3))
        throw std::runtime_error("store not admitted");
    const Json basis{{"work","deposit"},{"sourceMessage",3},{"observation","box"},{"item","cobblestone"},{"requested",8},
        {"cell",{{"x",2},{"y",-60},{"z",-2}}},{"before",{{"sampledAtMs",12}}},{"state","pending"}};
    Json original{{"session",1},{"order",3},{"skill","store_item"},{"bridgeWork","deposit"},{"bridgeExited",true},{"factAdmission",0},
        {"effect",DFY_V2_TOOL_UNKNOWN},{"result",{{"status","blocked"},{"deposit",basis}}}};
    if (facts.report(1,"mc.partner.action",original) || facts.reserve("check","observe",4))
        throw std::runtime_error("interrupted store not recorded");
    Json checked{{"session",1},{"order",4},{"skill","observe"},{"bridgeWork","check"},{"bridgeExited",true},{"factAdmission",0},
        {"effect",DFY_V2_TOOL_NOT_APPLIED},{"result",{{"chestReceipt",basis}}}};
    auto& proof = checked["result"]["chestReceipt"];
    proof.update(Json{{"state","verified"},{"stored",8},{"remaining",0},{"after",{{"sampledAtMs",15}}}});
    proof["cell"]["z"] = 2;
    if (facts.report(1,"mc.partner.action",checked)!=DFY_V2_INVALID_ARGUMENT) throw std::runtime_error("another chest settled work");
    proof["cell"] = basis.at("cell");
    proof["before"]["sampledAtMs"] = 13;
    if (facts.report(1,"mc.partner.action",checked)!=DFY_V2_INVALID_ARGUMENT) throw std::runtime_error("substituted baseline settled work");
    proof["before"] = basis.at("before");
    if (facts.report(1,"mc.partner.action",checked)) throw std::runtime_error("original store recheck failed");
    const auto watermark = works.watermark();
    const auto rows = works.view(1,0);
    const auto deposit = std::ranges::find_if(rows,[](const Json& row) { return row.at("work") == "deposit"; });
    if (deposit == rows.end() || deposit->at("revision") != 4 || deposit->at("effect").at("stored") != 8 ||
        !deposit->value("finalEffect",false)) throw std::runtime_error("original store is not finalized");
    if (facts.report(1,"mc.partner.action",checked) || works.watermark() != watermark)
        throw std::runtime_error("duplicate store settlement");
}

// 在原会话退役后核清同一投物的全部去向，分别覆盖正常退出和迟到退出。
// 步骤：接纳原件并模拟离场 → 验证作用域及未核清收件 → 按原号登记实收与其他收取。
void retired_pickup(bool unknown_exit) {
    dfy::app::BindingWorks works; ledger = &works;
    dfy_v2_agent_ordered_facts_api api{{{},submit,legacy},record,nullptr};
    dfy::mc::PartnerFacts facts;
    facts.configure(&api.base,"paired",&api);
    std::uint64_t request = 0;
    if (facts.submit(1,"paired/world",{},Json::object(),false,request) || facts.reserve("give","give_item",2))
        throw std::runtime_error("original work not admitted");
    Json action{{"session",1},{"order",2},{"skill","give_item"},{"bridgeWork","give"},{"bridgeExited",!unknown_exit},{"factAdmission",0},
        {"effect",DFY_V2_TOOL_APPLIED},{"result",{{"status","interrupted"},{"receipt",{{"source","server_events"},{"throwUnknown",0},{"pickupUnknown",8}}}}}};
    if (facts.report(1,"mc.partner.action",action)) throw std::runtime_error("original handoff missing");
    auto old = facts.retire_settlements();
    facts.clear();
    action["bridgeExited"] = true;
    if (old->report(1,"mc.partner.action",action) || !old->needs_pickup(1,"give")) throw std::runtime_error("retirement dropped original throws");
    Json sample{{"work","give"},{"recipient","paired"},{"item","stone"},{"requested",10},{"submitted",8},{"originalStatus","interrupted"},
        {"sourceThrough",12},{"receipt",{{"source","server_events"},{"recipientPicked",6},{"otherPicked",0},{"stream","s"},{"throwUnknown",0},{"pickupUnknown",2}}}};
    if (old->settle_pickup(2,"give",sample)!=DFY_V2_INVALID_CONTEXT || old->settle_pickup(1,"wrong",sample)!=DFY_V2_INVALID_CONTEXT ||
        old->settle_pickup(1,"give",sample) || !old->needs_pickup(1,"give")) throw std::runtime_error("scope or incomplete custody lost");
    sample["receipt"]["pickupUnknown"] = 0; sample["receipt"]["otherPicked"] = 2;
    if (old->settle_pickup(1,"give",sample) || old->needs_pickup(1,"give") || !works.finalized()) throw std::runtime_error("split custody unresolved");
    const auto head = works.watermark();
    const auto item = works.view(1,0).at(0);
    if (old->settle_pickup(1,"give",sample) || head!=works.watermark() || item.at("revision")!=(unknown_exit ? 5 : 4) ||
        item.at("effect").at("recipientPicked")!=6 || item.at("effect").at("otherPicked")!=2 || item.at("effect").at("submitted")!=8)
        throw std::runtime_error("retired work overstated actual receipt or repeated a transfer");
}
}

// 验证满额和退役时对原投物的查证，覆盖旧事实接口与迟到退出。
int main() {
    try { late_pickup(false,false); late_pickup(true,false); late_pickup(true,true); retired_pickup(false); retired_pickup(true); late_deposit(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
