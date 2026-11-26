#include "binding_facts.hpp"
#include <dafeiyu/agent_abi_v2.h>
#include <iostream>

namespace {
using dfy::app::BindingWorks;
using dfy::plugins::SubagentWorkFact;
using Json = nlohmann::json;

// 报告原工作身份、实效或退出水位与期望不符。
void check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }

// 确认旧号、冲突或断序均未产生新的回执。
void rejects(const auto& operation) {
    try { operation(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("unexpected work transition admitted");
}

// 固定玩家状态只保留在绑定镜像，增量、关闭和新代次不会留下重复的正文。
void status_samples() {
    dfy::app::BindingFacts facts;
    dfy::plugins::SubagentInput input;
    input.frame = dfy::plugins::SubagentFrame{3,1,"player/alpha",true};
    const Json sample{{"key","sample.stage"},{"version",1},{"subject","goal"},{"fact",8},{"value",{{"phase","ready"}}}};
    auto set = Json{{"event","entered"},{"decisionStatusSamples",Json::array({sample})}};
    input.prompt = Json{{"set",set},{"closed",Json::array()}}.dump();
    auto prompt = Json::parse(facts.apply(input,1,true));
    check(!prompt.contains("decisionStatusSamples") && prompt.at("event")=="entered" &&
        Json::parse(facts.status_samples())==Json::array({sample}),"status leaked into domain data or was lost");
    input.frame = dfy::plugins::SubagentFrame{3,2,"player/alpha",false};
    input.prompt = R"({"set":{"event":"again"},"closed":[]})";
    (void)facts.apply(input,2,false);
    check(Json::parse(facts.status_samples())==Json::array({sample}),"omitted sample lost on delta");
    input.frame->sequence = 3;
    input.prompt = R"({"set":{"decisionStatusSamples":{}},"closed":[]})";
    rejects([&] { facts.apply(input,3,false); });
    input.prompt = R"({"set":{},"closed":["decisionStatusSamples"]})";
    prompt = Json::parse(facts.apply(input,3,false));
    check(prompt.at("factFrame").at("closed").empty() && facts.status_samples()=="[]","closed sample remained visible");
}

// 同一工作经过多次实效更新后仍能登记真实退出，并继续查证迟到实收。
// 步骤：填满旧修订窗口 → 以第十七版确认退出 → 继续上报实效 → 重试及冲突后核对原号。
void late_revisions() {
    BindingWorks works;
    const SubagentWorkFact first{7,1,"give-original",R"({"source":12})",DFY_V2_WORK_ACCEPTED};
    const auto accepted = works.settle(7,first);
    const auto effect = [](std::uint64_t revision) {
        return SubagentWorkFact{7,revision,"give-original",Json{{"picked",revision}}.dump(),DFY_V2_WORK_EFFECT};
    };
    for (unsigned revision=2;revision<=16;++revision) works.settle(7,effect(revision));
    const SubagentWorkFact exit{7,17,"give-original",R"({"exited":true})",DFY_V2_WORK_EXIT};
    const auto exited = works.settle(7,exit);
    for (unsigned revision=18;revision<=45;++revision) works.settle(0,effect(revision));

    const auto watermark = works.watermark();
    check(works.settle(0,exit)==exited && works.settle(0,first)==accepted && works.settle(0,effect(45))==watermark,
        "original acceptance, exit or latest effect lost after rollover");
    rejects([&] { works.settle(7,effect(2)); });
    auto conflict = effect(45); conflict.content = "{}";
    rejects([&] { works.settle(7,conflict); });
    rejects([&] { works.settle(7,effect(47)); });
    const auto item = works.view(7,0).at(0);
    check(works.watermark()==watermark && item.at("revision")==45 && item.at("exited")==exited &&
        item.at("accepted")==accepted && item.at("effect").at("picked")==45 && works.view(8,0).empty(),
        "revision window changed identity, erased exit or leaked across epochs");
}

// 预算受限时保持在途工作的正文，外部名字大量转义也能用数字接纳号定位。
// 步骤：登记同规模在途和退出工作 → 用较小预算投影 → 核对剩余正文、原接纳号及过滤水位。
void bounded_view() {
    BindingWorks works;
    const auto body = Json{{"value",std::string(700,'v')}}.dump();
    const auto long_id = std::string(120,'\x01');
    const auto first = works.settle(1,{1,1,long_id,"{}",DFY_V2_WORK_ACCEPTED});
    works.settle(1,{1,2,long_id,body,DFY_V2_WORK_EFFECT});
    const auto exit = works.settle(1,{1,3,long_id,"{}",DFY_V2_WORK_EXIT});
    works.settle(1,{1,1,"pending","{}",DFY_V2_WORK_ACCEPTED});
    works.settle(1,{1,2,"pending",body,DFY_V2_WORK_EFFECT});

    const auto view = works.view(1,0,1200);
    check(view.size()==2 && view.dump().size()<=1200 && view[0].at("accepted")==first && view[0].at("exited")==exit &&
        view[0].contains("workSha256") && view[0].at("effect").at("bytes")==body.size() &&
        view[1].at("effect").at("value")==std::string(700,'v') && works.view(1,exit,1200).size()==1,
        "bounded work view lost an original reference or prioritized a settled body over a pending one");
}

// 为有序提交构造不可重用的首次接纳编号，后续版本始终沿原工作号。
SubagentWorkFact admission(std::uint64_t number) {
    return {1,1,"work-"+std::to_string(number),"{}",DFY_V2_WORK_ACCEPTED,number};
}

// 工作满额时只归档实效最终且已退出的原件，归档窗口外的重传也不能开启新工作。
// 步骤：登记三类工作并填满索引 → 查回归档的同号回执 → 再越过重传窗口 → 结算始终保留的未决工作。
void archive_completed() {
    BindingWorks works;
    std::uint64_t original = 0, effect_receipt = 0;
    for (unsigned number=1;number<=390;++number) {
        const auto first = admission(number);
        const auto accepted = works.settle(1,first);
        const auto effect = works.settle(1,{1,2,first.work,R"({"amount":8})",DFY_V2_WORK_EFFECT,0,number!=1});
        if (number!=2) works.settle(1,{1,3,first.work,"{}",DFY_V2_WORK_EXIT});
        if (number==3) { original = accepted; effect_receipt = effect; }
        if (number==129) {
            const auto before = works.watermark();
            check(works.settle(1,admission(3))==original &&
                works.settle(0,{1,2,"work-3",R"({"amount":8})",DFY_V2_WORK_EFFECT,0,true})==effect_receipt,
                "archived retransmission changed its receipt");
            auto changed = admission(3); changed.content = R"({"other":1})";
            rejects([&] { works.settle(1,changed); });
            rejects([&] { works.settle(1,{1,4,"work-3","{}",DFY_V2_WORK_EFFECT}); });
            check(works.watermark()==before,"archived read advanced the live index");
            const auto archived = Json::parse(works.read(1,effect_receipt).value());
            check(archived.at("archived")==true && archived.at("work")=="work-3" && archived.contains("sha256") &&
                !archived.contains("body") && !works.read(2,effect_receipt),"archived reference lost its scope or invented a body");
        }
    }
    rejects([&] { works.settle(1,admission(3)); });
    check(!works.read(1,effect_receipt),"evicted archive still exposed a body or a new identity");
    auto unordered = admission(3); unordered.admission_sequence = 0;
    rejects([&] { works.settle(1,unordered); });
    works.settle(0,{1,4,"work-1",R"({"received":8})",DFY_V2_WORK_EFFECT,0,true});
    works.settle(0,{1,3,"work-2","{}",DFY_V2_WORK_EXIT});
    const auto view = works.view(1,0);
    check(view.size()==128 && view[0].at("work")=="work-1" && view[0].at("effect").at("received")==8 &&
        view[0].at("finalEffect")==true,"pending pickup or actual exit was evicted");
}

// 所有槽位都在等待实效或退出时拒绝新原件；原工作补齐后可按同一序号再试。
// 步骤：填满待结算工作 → 验证拒绝不耗用水位 → 沿原号补齐回执并重试。
void pending_capacity() {
    BindingWorks works;
    for (unsigned n=1;n<=128;++n) works.settle(1,admission(n));
    bool full = false;
    try { works.settle(1,admission(129)); } catch (const std::length_error&) { full = true; }
    check(full && works.watermark()==128,"pending work evicted to admit a new action");
    works.settle(1,{1,2,"work-1","{}",DFY_V2_WORK_EXIT});
    works.settle(1,{1,3,"work-1","{}",DFY_V2_WORK_EFFECT,0,true});
    check(works.settle(1,admission(129))==131,"capacity rejection consumed the new work number");
}
}

// 以真实宿主索引回放工作数量及修订边界。
int main() {
    try { status_samples(); late_revisions(); bounded_view(); archive_completed(); pending_capacity(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
