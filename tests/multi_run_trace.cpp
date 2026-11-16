#include "support/scripted_model.hpp"
#include <dafeiyu/run_trace.hpp>
#include <algorithm>
#include <iostream>
#include <thread>

namespace {
using namespace dfy::agent;
using namespace dfy::model;
using namespace dfy::runtime;
using namespace dfy::tests;
// 报告多步节点投影、版本或保留窗口不一致。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 有界推进正式会话，不依赖查询产生进展。
void until(Session& session, const auto& predicate) {
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) { session.advance(); std::this_thread::yield(); }
    check(predicate(),"trace scenario stalled");
}
// 验证等待与完整多步快照、真实版本绑定和历史裁剪后的当前状态恢复。
void timeline() {
    unsigned actions = 0; ToolCatalog catalog;
    catalog.add({"fixture/action",17,3,{"action","fixture",R"({"type":"object","properties":{},"additionalProperties":false})"},true,false},
        [&](const auto&) { ++actions; return completed_tool({true,ToolEffect::applied,"private result",ToolError::none}); });
    ScriptStep first; first.last_text = "private goal"; first.tool_names = {"action"}; first.response = Response{{Role::assistant,"",{{"call","action","{}"}}},FinishReason::tool_calls};
    ScriptStep last; last.last_role = Role::tool; last.last_tool_id = "call"; last.tool_names = {"action"}; last.response = Response{{Role::assistant,"done"}};
    ScriptedModel model({1,"multi_trace",1,{},{first,last},2}); ConfirmationCaller caller;
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    Config config; config.provider = "qwen"; config.model = "offline-fixture"; Session session(config,channel,model.backend(),1);
    const std::vector<std::string> ids{"fixture/action"}; RunOptions options; options.tools = catalog.snapshot(ids,true); options.now = model.clock();
    options.confirmation_caller = caller; options.policy.confirm_changes = true; options.policy.revision = 7; options.trace_capacity = 3;
    options.version_labels.application_commit = "fixture-commit";
    auto request = session.begin("private goal",options); until(session,[&] { return !request->confirmations().empty(); });
    const auto waiting = run_timeline(*request,"test-boot");
    check(waiting.nodes.size() == 6 && !actions && waiting.nodes.back().confirmation == ConfirmationState::waiting &&
        !waiting.nodes[1].result && !waiting.nodes[2].exit,"query executed work or fabricated grouping status");
    const auto pending = request->confirmations().front(); session.decide(caller,{1,pending.id,pending.revision,pending.target,true});
    until(session,[&] { return request->exit()->state() == OperationState::succeeded; }); model.verify_complete();
    const auto result = run_timeline(*request,"test-boot");
    check(actions == 1 && result.nodes.size() == 9 && result.delta.gap && result.nodes[0].result == OperationState::succeeded &&
        result.versions.application_commit == "fixture-commit" && result.versions.policy == std::uint64_t{7} && result.versions.provider == "qwen" &&
        result.versions.tools[0].instance == DomainId{17} && result.versions.tools[0].contract == std::uint32_t{3} && !result.versions.build && !result.versions.tools[0].schema,
        "full snapshot lost current nodes or invented versions after trimming");
    for (const auto& node : result.nodes) {
        if (node.parent) check(std::any_of(result.nodes.begin(),result.nodes.end(),[&](const auto& parent) { return parent.id == *node.parent; }),"missing parent");
        if (node.id.kind == TraceNodeKind::model) check(node.observed_exit_ns && !node.usage,"missing actual exit or invented model usage");
    }
    check(run_timeline(*request,"test-boot",result.delta.latest_revision).delta.facts.empty() && actions == 1,"snapshot query replayed action");
    request.reset(); session.clear(); check(result.nodes[0].result == OperationState::succeeded,"retained value snapshot changed");
}
}
// 用固定模型结果验证完整元数据，测试不连接真实供应商。
int main() {
    try { timeline(); std::cout << "Multi-run trace checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
