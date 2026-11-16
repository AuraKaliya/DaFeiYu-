#include "support/scenario_replay.hpp"
#include <iostream>

namespace {
using namespace dfy::agent;
using namespace dfy::model;
using namespace dfy::runtime;
using namespace dfy::tests;
// 报告受控重跑与明确场景契约不符。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 核对缺少材料和场景矛盾必须失败，不把它们计作成功运行。
void rejects(const auto& action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid replay accepted");
}
// 建立一个带人工确认的固定工具场景，所有模型、工具及预期都明确列出。
ReplayCase base() {
    ScriptStep first; first.last_text = "goal"; first.tool_names = {"counter"}; first.response = Response{{Role::assistant,"",{{"one","counter","{}"}}},FinishReason::tool_calls};
    ScriptStep last; last.last_role = Role::tool; last.last_tool_id = "one"; last.tool_names = {"counter"}; last.response = Response{{Role::assistant,"done"}};
    ReplayCase value; value.model = {1,"confirmed_counter",1,{"fixture/counter"},{first,last},2}; value.prompt = "goal";
    value.tools.push_back({{"fixture/counter",1,1,{"counter","fixed fixture",R"({"type":"object","properties":{},"additionalProperties":false})"},true,false},
        {{true,ToolEffect::applied,"1",ToolError::none}}});
    value.policy.confirm_changes = true; value.decisions = {ReplayChoice::approve};
    value.expected = {OperationState::succeeded,PolicyReason::none,1,4,{ToolEffect::applied}}; return value;
}
// 验证同一固定场景重复运行具有相同动作事实，同时运行身份保持独立。
void reproducible() {
    const auto first = replay_scenario(base()), second = replay_scenario(base());
    check(first.tool_calls == second.tool_calls && first.effects == second.effects && first.model.consumed == second.model.consumed &&
        first.trace.key.run != second.trace.key.run && first.trace.versions.model_profile == "offline-fixed-v1","replay merged identity or changed facts");
    auto failure = base(); failure.model.steps[1].response.reset(); failure.model.steps[1].fail = true;
    failure.expected.result = OperationState::failed; failure.expected.history_messages = 3;
    check(replay_scenario(failure).effects == std::vector<ToolEffect>{ToolEffect::applied},"later model failure erased replay effect");
}
// 验证拒绝跨模型调用持续生效，取消和到期均不变成隐式批准。
void decisions() {
    auto denied = base(); denied.tools[0].results.clear(); denied.decisions = {ReplayChoice::deny};
    auto repeated = denied.model.steps[1]; repeated.response = denied.model.steps[0].response; repeated.response->message.tool_calls[0].id = "two";
    auto final = denied.model.steps[1]; final.last_tool_id = "two"; denied.model.steps = {denied.model.steps[0],repeated,final}; denied.model.expected_model_requests = 3;
    denied.expected = {OperationState::succeeded,PolicyReason::none,0,6,{ToolEffect::not_applied,ToolEffect::not_applied}};
    const auto result = replay_scenario(denied); check(result.tool_calls == 0,"renamed candidate bypassed denial");
    for (const auto choice : {ReplayChoice::cancel,ReplayChoice::expire}) {
        auto value = base(); value.tools[0].results.clear(); value.model.steps.resize(1); value.model.expected_model_requests = 1; value.decisions = {choice};
        value.expected = {OperationState::cancelled,choice == ReplayChoice::cancel ? PolicyReason::cancelled : PolicyReason::deadline,0,3,{ToolEffect::not_applied}};
        check(!replay_scenario(value).tool_calls,"cancelled replay executed tool");
    }
}
// 验证未知夹具、缺少模型／工具响应和控制事件不能降级到真实执行或意外通过。
void missing_material() {
    auto value = base(); value.model.schema_version = 2; rejects([&] { replay_scenario(value); });
    value = base(); value.model.fixtures = {"production/plugin"}; rejects([&] { replay_scenario(value); });
    value = base(); value.decisions.clear(); rejects([&] { replay_scenario(value); });
    value = base(); value.tools[0].results.clear(); value.expected.result = OperationState::failed; rejects([&] { replay_scenario(value); });
    value = base(); value.model.steps.resize(1); value.model.expected_model_requests = 1; value.expected.result = OperationState::failed;
    rejects([&] { replay_scenario(value); });
}
}
// 只重跑固定 C++ 场景和内存夹具，不读取诊断文件中的执行入口。
int main() {
    try { reproducible(); decisions(); missing_material(); std::cout << "Scenario replay checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
