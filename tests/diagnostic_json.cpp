#include "support/scripted_model.hpp"
#include <dafeiyu/diagnostic_json.hpp>
#include <nlohmann/json.hpp>
#include <iostream>
#include <thread>

namespace {
using namespace dfy::agent;
using namespace dfy::model;
using namespace dfy::runtime;
using namespace dfy::tests;
// 报告诊断文件泄露正文或篡改原事实。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 取得有明确私有正文的已完成计划，返回的诊断不保留原 Session。
DiagnosticSnapshot fixture() {
    ScriptStep generation; generation.response = Response{{Role::assistant,R"({"schema_version":1,"steps":[{"id":"one","objective":"PRIVATE_OBJECTIVE","completion_criterion":"PRIVATE_CRITERION","tools":[]}]})"}};
    ScriptStep answer; answer.response = Response{{Role::assistant,"PRIVATE_ANSWER"}};
    ScriptedModel model({1,"diagnostic_json",1,{},{generation,answer},2});
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    Session session({},channel,model.backend()); ConfirmationCaller caller; RunOptions options;
    options.planning = PlanLimits{}; options.confirmation_caller = caller; options.now = model.clock(); options.trace_capacity = 3;
    const auto request = session.begin("PRIVATE_GOAL",options); const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while ((!request->plan()->definition || request->plan()->steps[0].waiting != PlanWait::verification) && std::chrono::steady_clock::now() < deadline) { session.advance(); std::this_thread::yield(); }
    check(request->plan()->steps.size() == 1 && request->plan()->steps[0].waiting == PlanWait::verification,"planning fixture stalled");
    session.confirm_plan_step(caller,request->plan()->steps[0].key,"PRIVATE_EVIDENCE"); session.advance(); model.verify_complete();
    check(!session.busy(),"fixture did not exit"); return diagnostic_snapshot(*request,"fixture-export");
}
}
// 编码固定模型场景的只读值，不进行网络或文件写入。
int main() {
    try {
        const auto value = fixture(); const auto original = value.timeline.origin; const auto text = encode_diagnostic(value); const auto json = nlohmann::json::parse(text);
        check(text.find("PRIVATE_") == std::string::npos && json["schema_version"] == 1 && json["timeline_schema_version"] == 4 &&
            json["data"]["timeline"]["versions"]["application_commit"].is_null() && !json["data"]["timeline"].contains("origin"),"diagnostic exposed content or invented versions");
        check(json["data"]["timeline"]["delta"]["gap"] == true && json["data"]["plans"][0]["steps"][0]["has_evidence"] == true &&
            json["data"]["plans"][0]["steps"][0]["state"] == "completed" && value.timeline.origin == original && text == encode_diagnostic(value),
            "encoding changed state, lost gap or was not deterministic");
        std::cout << "Diagnostic JSON checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
