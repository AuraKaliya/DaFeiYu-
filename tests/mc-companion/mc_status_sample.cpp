#include "../../plugins/mc/src/partner_facts.hpp"
#include "../../plugins/mc/src/partner_task.hpp"
#include "binding_facts.hpp"
#include <dafeiyu/decision_status.hpp>
#include <dafeiyu/tool_abi_v2.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
using Json = nlohmann::json;
Json frames;
bool request_baseline = false;
dfy::app::BindingWorks* pickup_ledger = nullptr;

// 将桥接拾取工作的 C ABI 原回执登记到真实的宿主工作索引。
dfy_v2_status DFY_V2_CALL settle_pickup(uint64_t session,const dfy_v2_work_fact* fact,uint64_t* receipt,dfy_v2_error*) {
    *receipt = pickup_ledger->settle(session,{fact->epoch,fact->revision,{fact->work.data,fact->work.length},
        {fact->content.data,fact->content.length},fact->phase,0,false});
    return DFY_V2_OK;
}

// 记录试点发出的原补丁和帧身份，可要求一次同号基线补发。
dfy_v2_status DFY_V2_CALL capture(uint64_t, const dfy_v2_agent_input* input, const dfy_v2_fact_frame* frame,
    uint64_t* request, uint64_t* expected, dfy_v2_error*) {
    frames.push_back({{"epoch",frame->epoch},{"scope",std::string(frame->scope.data,frame->scope.length)},{"sequence",frame->sequence},
        {"flags",frame->flags},{"patch",Json::parse(input->prompt.data,input->prompt.data+input->prompt.length)}});
    if (request_baseline) {
        request_baseline = false;
        *expected = frame->sequence;
        return DFY_V2_NOT_FOUND;
    }
    *request = frame->sequence;
    return DFY_V2_OK;
}

// 在同一玩家绑定内撤去来源并重新接纳任务，用真实宿主镜像和冻结快照核对结果。
// 步骤：发布首个任务 → 分别删除任务、清空目标或失去玩家 → 重传基线并接纳新修订 → 查回旧版原件。
void verify_withdrawal(const Json& definitions) {
    constexpr std::uint64_t session = 7;
    const std::string player = "fff01234-1234-1234-1234-123456789abc";
    dfy_v2_agent_facts_api api{{},capture,nullptr};
    dfy::mc::PartnerFacts facts;
    facts.configure(&api,player);
    dfy::app::BindingFacts mirror;
    dfy::agent::DecisionStatusStore snapshots;
    // 沿原 ABI 提交并只合入接纳的帧，保留每次实际发布的状态供按版查询。
    // 步骤：取得原补丁 → 合并宿主镜像 → 冻结并发布本步快照。
    const auto send = [&](const Json& scene) {
        std::uint64_t request = 0;
        if (facts.submit(session,player+"/world",{},scene,false,request)) throw std::runtime_error("goal frame rejected");
        const auto& frame = frames.back();
        dfy::plugins::SubagentInput input;
        input.prompt = frame.at("patch").dump();
        input.frame = dfy::plugins::SubagentFrame{frame.at("epoch"),frame.at("sequence"),frame.at("scope"),frame.at("flags") == DFY_V2_FACT_BASELINE};
        mirror.apply(input,request,false);
        auto frozen = snapshots.prepare(session,mirror.epoch(),request,1,100,{{"mc.companion",definitions.dump(),mirror.status_samples()}});
        auto tail = Json::parse(frozen.tail);
        snapshots.publish(std::move(frozen));
        return tail;
    };
    dfy::mc::PartnerTask task;
    Json scene{{"player",player},{"task",task.context()}};
    if (!send(scene).at("items").empty()) throw std::runtime_error("initial binding fabricated a goal");
    task.update({{"goal","清理门边"},{"next","player"}},scene,0,nullptr,2);
    scene["task"] = task.context();
    const auto first = send(scene);
    const auto snapshot = first.at("snapshot").get<std::string>();
    const auto original = snapshots.read(session,session,snapshot);
    const auto& items = first.at("items");
    if (items.size() != 1 || items.at(0).at("fact") != 1 || items.at(0).at("subject") != player ||
        items.at(0).at("value").at("goal") != "清理门边" || Json::parse(original).at("status").at("items") != items)
        throw std::runtime_error("first snapshot lost the accepted goal");

    // 核对同一绑定的撤样帧和恢复帧，已发布的空快照仍可按原号读回。
    // 步骤：检查当帧关闭或清空 → 重复缺样不重发 → 恢复原修订 → 读回撤样快照。
    const auto withdraw_and_restore = [&](const Json& absent, const char* reason) {
        const auto withdrawn = send(absent);
        const auto& patch = frames.back().at("patch");
        const auto& closed = patch.at("closed");
        const bool closes_samples = std::ranges::find(closed,"decisionStatusSamples") != closed.end();
        const auto samples = patch.at("set").find("decisionStatusSamples");
        const bool clears_samples = samples != patch.at("set").end() && *samples == Json::array();
        if ((!closes_samples && !clears_samples) || !withdrawn.at("items").empty() || frames.back().at("flags") != 0)
            throw std::runtime_error(std::string("same-binding delta did not withdraw goal: ")+reason);
        if (!send(absent).at("items").empty() || frames.back().at("patch").at("set").contains("decisionStatusSamples") ||
            !frames.back().at("patch").at("closed").empty()) throw std::runtime_error("repeated absence produced another status patch");
        if (send(scene).at("items") != items) throw std::runtime_error("restored source lost its original goal fact");
        const auto empty_id = withdrawn.at("snapshot").get<std::string>();
        if (!Json::parse(snapshots.read(session,session,empty_id)).at("status").at("items").empty())
            throw std::runtime_error("restoring the goal rewrote a withdrawn snapshot");
    };
    for (const auto field : {"task","player"}) {
        auto absent = scene;
        absent.erase(field);
        withdraw_and_restore(absent,field);
    }
    auto absent = scene;
    absent["player"] = "bbb01234-1234-1234-1234-123456789abc";
    withdraw_and_restore(absent,"other player");
    absent["player"] = nullptr;
    withdraw_and_restore(absent,"null player");
    absent = scene;
    absent["task"]["goal"] = "";
    withdraw_and_restore(absent,"cleared goal");

    request_baseline = true;
    if (!send(absent).at("items").empty() || frames.back().at("flags") != DFY_V2_FACT_BASELINE ||
        frames.back().at("sequence") != frames.at(frames.size()-2).at("sequence") ||
        frames.back().at("patch").at("set").contains("decisionStatusSamples"))
        throw std::runtime_error("baseline retry retained a withdrawn sample");
    task.update({{"goal","转去修墙"},{"next","player"}},scene,0,nullptr,3);
    scene["task"] = task.context();
    const auto next = send(scene).at("items");
    if (next.size() != 1 || next.at(0).at("fact") != 2 || next.at(0).at("value").at("goal") != "转去修墙" ||
        snapshots.read(session,session,snapshot) != original) throw std::runtime_error("new admission changed a frozen goal");
}

// 核对原候选的持续关注、终态接纳、断序恢复和隔离回合，失效绑定不能发布旧唤醒。
// 步骤：接纳等待候选 → 接纳同号终态并拦重 → 重取基线 → 切换作用域并核对关闭。
void verify_approval() {
    const std::string player = "player";
    dfy_v2_agent_facts_api api{{},capture,nullptr};
    dfy::mc::PartnerFacts facts;
    facts.configure(&api,player);
    Json scene{{"player",player},{"pendingApproval",{{"status","waiting"},{"work","work-7"},{"item",42},{"order",2},{"skill","collect"}}}};
    const Json result{{"player",player},{"work","work-7"},{"id",42},{"order",2},{"skill","collect"},
        {"status","withdrawn"},{"source","host"},{"execution","skipped"},{"generation","world"}};
    // 发出实际绑定补丁并取回本次接纳的传输内容。
    const auto send = [&](std::uint64_t session, std::string_view scope, bool isolated = false) {
        std::uint64_t request = 0;
        if (facts.submit(session,scope,{},scene,isolated,request)) throw std::runtime_error("approval frame rejected");
        return frames.back().at("patch").at("set");
    };
    const auto waiting = send(7,"world/1");
    const auto& sample = waiting.at("decisionStatusSamples").at(0);
    const auto& task = waiting.at("decisionStatusAttention").at(0);
    if (sample.at("fact") != 42 || sample.at("subject") != "work-7" || sample.at("key") != "mc.companion.pending_approval" ||
        task.at("mode") != "continuous" || task.at("task") != "work-7") throw std::runtime_error("lost waiting work reference");
    scene.erase("pendingApproval");
    auto other = result; other["player"] = "visitor";
    if (facts.accept_approval(8,52,81,result) || facts.accept_approval(7,52,81,other) || facts.accept_approval(7,52,0,result) ||
        !facts.accept_approval(7,52,81,result) || facts.accept_approval(7,52,81,result)) throw std::runtime_error("approval admission identity");
    const auto settled = send(7,"world/1");
    const auto& watch = settled.at("decisionStatusAttention").at(0);
    const auto& terminal = settled.at("decisionStatusSamples").at(0);
    if (watch.at("mode") != "counted" || watch.at("count") != 1 || watch.at("event") != 81 ||
        terminal.at("fact") != 81 || terminal.at("value").at("mailbox") != 52 || terminal.at("value").at("execution") != "skipped" ||
        terminal.at("subject") != sample.at("subject")) throw std::runtime_error("terminal lost the original receipt or work");
    request_baseline = true;
    if (send(7,"world/1").at("decisionStatusAttention") != settled.at("decisionStatusAttention") ||
        send(7,"world/1",true).contains("decisionStatusSamples") ||
        send(7,"world/1").at("decisionStatusSamples") != settled.at("decisionStatusSamples") ||
        send(7,"world/2").contains("decisionStatusSamples") || send(8,"world/2").contains("decisionStatusAttention") ||
        facts.accept_approval(7,52,82,result)) throw std::runtime_error("approval escaped its original scope");
}

// 从真实桥接夹具的标准输入读取三个原工作结果，对照宿主实效回执和实际状态快照。
// 步骤：建立玩家绑定 → 接纳原回执并用包声明冻结 → 对照库存和余量 → 结束任务后解除关注。
void verify_pickup(const std::filesystem::path& package) {
    std::ifstream file(package/"agent/status.json");
    const auto definitions = Json::parse(file).at("definitions").dump();
    const auto receipts = Json::parse(std::cin);
    if (receipts.size()!=3) throw std::runtime_error("three original pickup receipts required");
    dfy::app::BindingWorks ledger; pickup_ledger = &ledger;
    dfy::app::BindingFacts mirror;
    dfy::agent::DecisionStatusStore store;
    dfy_v2_agent_facts_api api{{},capture,settle_pickup};
    dfy::mc::PartnerFacts facts; facts.configure(&api,"paired");
    Json scene{{"player","paired"},{"task",{{"revision",4},{"next","continue"}}}};
    std::uint64_t request = 0;
    // 合入本绑定的真实帧，以同一份包声明校验完整快照和当步尾注。
    const auto send = [&] {
        if (facts.submit(7,"paired/world",{},scene,false,request)) throw std::runtime_error("pickup frame rejected");
        const auto& frame = frames.back();
        dfy::plugins::SubagentInput input;
        input.prompt = frame.at("patch").dump();
        input.frame = dfy::plugins::SubagentFrame{7,frame.at("sequence"),frame.at("scope"),frame.at("flags")==DFY_V2_FACT_BASELINE};
        mirror.apply(input,request,false);
        auto frozen = store.prepare(7,7,request,1,1700000000000,{{"mc.companion",definitions,mirror.status_samples(),mirror.status_attention()}});
        const auto tail = Json::parse(frozen.tail); store.publish(std::move(frozen)); return tail;
    };
    if (!send().at("items").empty()) throw std::runtime_error("pickup fabricated before original effect");
    for (std::size_t i=0;i<receipts.size();++i) {
        const auto work = "pickup-"+std::to_string(i);
        auto raw = receipts.at(i);
        const auto& proof = raw.at("pickup").at("effect");
        if (i==1) raw["largeObservation"] = std::string(7200,'x');
        Json action{{"session",7},{"order",2},{"skill","pickup_drop"},{"bridgeWork",work},{"bridgeExited",true},{"factAdmission",0},
            {"effect",DFY_V2_TOOL_APPLIED},{"taskRevision",4},{"result",raw}};
        if (facts.reserve(work,"pickup_drop",2) || facts.report(7,"mc.partner.action",action)) throw std::runtime_error("pickup admission failed");
        const auto tail = send();
        const auto& item = tail.at("items").at(0);
        const auto original = Json::parse(ledger.read(7,item.at("fact")).value());
        const auto& body = original.at("body");
        const auto& source = body.contains("pickupEffect") ? body.at("pickupEffect") : body.at("result").at("pickup").at("effect");
        auto value = item.at("value"); value["sampledUtcMs"] = item.at("sampledUtcMs");
        if (tail.at("items").size()!=1 || item.at("subject")!=work || original.at("work")!=work || original.at("revision")!=2 ||
            item.at("attention").at("tasks")!=Json::array({"task/4"}) || value!=source || source!=proof ||
            value.at("acquired")!=3 || value.at("target")!=8 || value.contains("pendingPickup")!=(i==0))
            throw std::runtime_error("pickup snapshot differs from original effect");
    }
    scene["task"]["next"] = "done";
    if (!send().at("items").empty()) throw std::runtime_error("finished task kept pickup explicit");
}

// 核对包声明与真实任务修订，并验证缺样、备注更新、强制基线和重绑。
// 步骤：读取实际包定义 → 接纳目标并核对原件 → 比对备注与同号补发 → 切换绑定。
void verify(const std::filesystem::path& package) {
    std::ifstream file(package/"agent/status.json");
    const auto catalog = Json::parse(file);
    const auto& definition = catalog.at("definitions").at(0);
    if (catalog.at("schema") != "dafeiyu.status_definitions/1" || definition.at("key") != "mc.companion.task_goal" ||
        definition.at("version") != 1 || definition.at("defaultAttention").at("mode") != "explicit" ||
        definition.at("valueSchema").at("required") != Json::array({"goal"}) ||
        definition.at("valueSchema").at("properties").at("goal").at("type") != "string")
        throw std::runtime_error("MC task goal definition differs from package contract");

    const std::string player = "fff01234-1234-1234-1234-123456789abc";
    dfy_v2_agent_facts_api api{{},capture,nullptr};
    dfy::mc::PartnerFacts facts;
    dfy::mc::PartnerTask task;
    facts.configure(&api,player);
    Json scene{{"player",player},{"lastObservation",{{"sampledAtMs",1234}}}};
    std::uint64_t request = 0;
    // 把当前真实任务经原接口发送，只读回已接纳帧的 set。
    const auto send = [&](std::uint64_t session = 1) -> Json {
        scene["task"] = task.context();
        if (facts.submit(session,player,{},scene,false,request)) throw std::runtime_error("MC sample rejected");
        return frames.back().at("patch").at("set");
    };
    if (send().contains("decisionStatusSamples")) throw std::runtime_error("missing goal sampled on initial baseline");
    task.update({{"goal","清理门边"},{"next","player"}},scene,0,nullptr,2);
    const auto first = send().at("decisionStatusSamples");
    const auto expected = Json::array({{{"key",definition.at("key")},{"version",definition.at("version")},{"subject",player},
        {"fact",task.revision()},{"value",{{"goal","清理门边"}}}}});
    if (task.revision() != 1 || first != expected) throw std::runtime_error("first goal lost original identity or fabricated a time");
    task.update({{"goal","清理门边"},{"detail","等玩家答复"},{"next","continue"}},scene,0,nullptr,2);
    if (send().contains("decisionStatusSamples") || task.revision() != 1) throw std::runtime_error("note resampled the goal");
    request_baseline = true;
    if (send().at("decisionStatusSamples") != first || frames.back().at("flags") != DFY_V2_FACT_BASELINE ||
        frames.back().at("sequence") != frames[frames.size()-2].at("sequence"))
        throw std::runtime_error("baseline recovery changed the original goal fact");
    task.update({{"goal","转去修墙"},{"next","player"}},scene,0,nullptr,3);
    const auto second = send().at("decisionStatusSamples").at(0);
    if (second.at("fact") != 2 || second.at("value").at("goal") != "转去修墙" || second.at("subject") != player)
        throw std::runtime_error("revised goal was not published");
    scene["player"] = "another-player";
    if (send(2).contains("decisionStatusSamples")) throw std::runtime_error("foreign player inherited sample");
    task = {};
    scene["player"] = player;
    if (send(3).contains("decisionStatusSamples")) throw std::runtime_error("new binding inherited old goal");
    verify_withdrawal(catalog.at("definitions"));
    verify_approval();
}
}

// 从本配置生成的实际 MC 包读取状态定义并验证选定玩家的事实补丁。
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 3 && std::wstring_view(argv[2])==L"--pickup") verify_pickup(argv[1]);
        else if (argc == 2) verify(argv[1]);
        else throw std::invalid_argument("MC package path required");
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
