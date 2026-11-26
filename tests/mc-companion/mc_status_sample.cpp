#include "../../plugins/mc/src/partner_facts.hpp"
#include "../../plugins/mc/src/partner_task.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
using Json = nlohmann::json;
Json frames;
bool request_baseline = false;

// 记录试点发出的原补丁和帧身份，可要求一次同号基线补发。
dfy_v2_status DFY_V2_CALL capture(uint64_t, const dfy_v2_agent_input* input, const dfy_v2_fact_frame* frame,
    uint64_t* request, uint64_t* expected, dfy_v2_error*) {
    frames.push_back({{"sequence",frame->sequence},{"flags",frame->flags},{"patch",Json::parse(input->prompt.data,input->prompt.data+input->prompt.length)}});
    if (request_baseline) {
        request_baseline = false;
        *expected = frame->sequence;
        return DFY_V2_NOT_FOUND;
    }
    *request = frame->sequence;
    return DFY_V2_OK;
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
}
}

// 从本配置生成的实际 MC 包读取状态定义并验证选定玩家的事实补丁。
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("MC package path required");
        verify(argv[1]);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
