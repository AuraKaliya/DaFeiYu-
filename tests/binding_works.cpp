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
}

// 以真实宿主索引回放工作数量及修订边界。
int main() {
    try { late_revisions(); bounded_view(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
