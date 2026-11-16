#pragma once
#include "scripted_model.hpp"
#include <dafeiyu/run_trace.hpp>
#include <thread>

namespace dfy::tests {
enum class ReplayChoice { approve, deny, cancel, expire };
// 仅声明固定结果夹具，无函数、插件路径、文件路径或网络入口。
struct ReplayTool { agent::ToolDescription description; std::vector<agent::ToolResult> results; };
struct ReplayExpected {
    runtime::OperationState result = runtime::OperationState::succeeded;
    agent::PolicyReason stop_reason = agent::PolicyReason::none;
    std::size_t tool_calls = 0, history_messages = 0;
    std::vector<agent::ToolEffect> effects;
};
struct ReplayCase {
    ScenarioSpec model;
    std::string prompt;
    std::vector<ReplayTool> tools;
    agent::PolicySettings policy;
    std::chrono::milliseconds timeout{1000};
    std::vector<ReplayChoice> decisions;
    ReplayExpected expected;
};
struct ReplayReport {
    ScriptReport model;
    std::size_t tool_calls = 0;
    std::vector<agent::ToolEffect> effects;
    agent::RunTimeline trace;
};
// 在全新正式 Session 中重跑显式固定场景，只允许内存结果夹具，失败不回退真实 Provider。
// 步骤：核对场景及夹具声明 → 建立固定模型和工具 → 按语义等待点提交决定 → 核对全部材料及业务事实。
inline ReplayReport replay_scenario(ReplayCase scenario) {
    using namespace agent;
    using namespace runtime;
    if (scenario.tools.size() > 64 || scenario.decisions.size() > 64) throw std::invalid_argument("replay: fixture or decision limit exceeded");
    std::vector<std::string> ids;
    for (const auto& tool : scenario.tools) {
        if (!tool.description.id.starts_with("fixture/") || tool.results.size() > 64) throw std::invalid_argument("replay: fixed fixture required");
        ids.push_back(tool.description.id);
    }
    auto declared = scenario.model.fixtures, registered = ids; std::sort(declared.begin(),declared.end()); std::sort(registered.begin(),registered.end());
    if (declared != registered) throw std::invalid_argument("replay: fixture declarations do not match");
    for (const auto choice : scenario.decisions) if (static_cast<unsigned>(choice) > static_cast<unsigned>(ReplayChoice::expire))
        throw std::invalid_argument("replay: unsupported decision");
    ScriptedModel model(scenario.model,ids); ToolCatalog catalog;
    struct ToolState { std::vector<ToolResult> results; std::size_t used = 0, violations = 0; };
    std::vector<std::shared_ptr<ToolState>> states;
    for (auto& tool : scenario.tools) {
        auto state = std::make_shared<ToolState>(); state->results = std::move(tool.results); states.push_back(state);
        // 消耗显式固定结果，任何额外调用都是场景违约，不能用工具失败掩盖它。
        // 步骤：核对剩余结果 → 标记越界或取出结果 → 包装为原工具工作。
        catalog.add(std::move(tool.description),[state](const auto&) {
            if (state->used == state->results.size()) { ++state->violations; throw std::runtime_error("replay: unexpected tool call"); }
            return completed_tool(state->results[state->used++]);
        });
    }
    auto channel = std::make_shared<events::BroadcastChannel>(std::make_shared<events::EventStore>(16,64),1,8);
    Session session({},channel,model.backend()); ConfirmationCaller caller; RunOptions options;
    if (!ids.empty()) options.tools = catalog.snapshot(ids,true);
    options.policy = scenario.policy; options.timeout = scenario.timeout; options.now = model.clock();
    options.confirmation_caller = caller; options.confirmation_limit = 64; options.version_labels.model_profile = "offline-fixed-v1";
    const auto request = session.begin(scenario.prompt,options); std::size_t decisions = 0;
    const auto watchdog = std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (request->exit()->state() == OperationState::pending && std::chrono::steady_clock::now() < watchdog) {
        session.advance(); const auto confirmations = request->confirmations();
        const auto waiting = std::find_if(confirmations.begin(),confirmations.end(),[](const auto& value) { return value.state == ConfirmationState::waiting; });
        if (waiting != confirmations.end()) {
            if (decisions == scenario.decisions.size()) throw std::runtime_error("replay: missing explicit confirmation decision");
            const auto choice = scenario.decisions[decisions++];
            if (choice == ReplayChoice::cancel) request->request_cancel(CancelReason::user);
            else if (choice == ReplayChoice::expire) model.advance_clock(scenario.timeout);
            else {
                const ConfirmationCommand command{decisions,waiting->id,waiting->revision,waiting->target,choice == ReplayChoice::approve};
                const auto receipt = session.decide(caller,command); const auto revision = request->trace_delta().latest_revision;
                if (session.decide(caller,command) != receipt || request->trace_delta().latest_revision != revision)
                    throw std::runtime_error("replay: repeated decision changed facts");
            }
        }
        std::this_thread::yield();
    }
    if (request->exit()->state() != OperationState::succeeded) throw std::runtime_error("replay: actual exit missing");
    model.verify_complete();
    if (decisions != scenario.decisions.size()) throw std::runtime_error("replay: unconsumed control events");
    ReplayReport report; report.model = model.report(); report.trace = run_timeline(*request,"offline-replay");
    for (const auto& state : states) {
        if (state->violations || state->used != state->results.size()) throw std::runtime_error("replay: tool fixture contract not satisfied");
        report.tool_calls += state->used;
    }
    for (const auto& tool : request->tool_steps()) report.effects.push_back(tool.effect);
    const auto& expected = scenario.expected;
    if (request->operation()->state() != expected.result || request->diagnostics().stop_reason != expected.stop_reason ||
        report.tool_calls != expected.tool_calls || report.effects != expected.effects || session.message_count() != expected.history_messages)
        throw std::runtime_error("replay: execution facts differ from explicit expectations");
    for (const auto& node : report.trace.nodes) if (node.parent && std::none_of(report.trace.nodes.begin(),report.trace.nodes.end(),
        [&](const auto& parent) { return parent.id == *node.parent; })) throw std::runtime_error("replay: missing trace parent");
    if (!review_timeline(report.trace).run_exited) throw std::runtime_error("replay: diagnostic review did not confirm actual exit");
    return report;
}
}
