#include "tool_verification.hpp"
#include <dafeiyu/plugin_manager.hpp>
#include <dafeiyu/plugin_tools.hpp>
#include <iostream>
#include <thread>

namespace {
using namespace dfy;
using runtime::OperationState;
namespace fs = std::filesystem;

// 在观察证明或状态作用与约定不同时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

// 创建本次验证独占的包及实例配置目录。
fs::path workspace(const fs::path& root) {
    fs::create_directories(root);
    for (unsigned i = 1;; ++i) {
        const auto path = root/std::to_string(i);
        if (fs::create_directory(path)) return path;
    }
}

// 只返回同一读、改、读任务所需的固定候选和普通文本。
// 步骤：产生原生计划 → 按顺序请求三个工具 → 给出未经证实的完成说明。
model::Response scripted(unsigned call, const model::Request& request) {
    using namespace model;
    if (call == 1) return {{Role::assistant,R"({"schema_version":1,"steps":[{"id":"mode","objective":"Set focus","completion_criterion":"Check actual mode","tools":["example.status/read","example.status/set_focus"]}]})"}};
    check(request.tools.size() == 2,"model did not receive the same two explicit grants");
    if (call == 2) return {{Role::assistant,"",{{"before",request.tools[0].name,"{}"}}},FinishReason::tool_calls};
    const auto& last = request.messages.back();
    check(last.role == Role::tool,"tool result was not paired before continuation");
    if (call == 3) return {{Role::assistant,"",{{"change",request.tools[1].name,"{}"}}},FinishReason::tool_calls};
    if (call == 4) return {{Role::assistant,"",{{"after",request.tools[0].name,"{}"}}},FinishReason::tool_calls};
    if (call == 5) return {{Role::assistant,"the requested mode has been set"}};
    throw std::runtime_error("verification invoked the model again");
}

// 通过真实面板操作验证模型输出与宿主确认的交界。
// 步骤：加载同一面板实例 → 通过基线审批 → 等待真实重读 → 核对匹配与不匹配的不同结算。
void verify(const fs::path& package, const fs::path& root, bool matches) {
    const auto run = workspace(root);
    auto channel = std::make_shared<events::BroadcastChannel>(std::make_shared<events::EventStore>(16,64),4,8);
    plugins::PluginManager manager(run/"plugins",run/"user",channel);
    manager.install(package);
    check(manager.load("example.status") == 0,"status package did not load");
    const auto instance = manager.instance("example.status");
    auto catalog = std::make_shared<agent::ToolCatalog>();
    plugins::PluginTools bindings(instance,catalog);
    const std::vector<std::string> ids{"example.status/read","example.status/set_focus"};
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(3);
    app::ToolVerification proof(ids[0],ids[1],matches ? "focus" : "idle",instance->generation(),deadline);

    unsigned calls = 0;
    agent::Session session({},channel,model::Backend{[&](const auto&, const auto& request) { return scripted(++calls,request); }});
    const agent::ConfirmationCaller caller;
    agent::RunOptions options;
    options.tools = catalog->snapshot(ids,true);
    options.planning = agent::PlanLimits{1,1};
    options.confirmation_caller = caller;
    options.policy.confirm_changes = true;
    options.policy.tool_limit = 3;
    options.deadline = deadline;
    const auto request = session.begin("set focus",std::move(options));

    bool decided = false;
    while (!proof.match() && std::chrono::steady_clock::now() < deadline) {
        proof.advance(session,*request,caller);
        const auto confirmations = request->confirmations();
        if (!decided && !confirmations.empty()) {
            const auto& pending = confirmations[0];
            check(proof.can_approve(pending.target,std::chrono::steady_clock::now()),"actual initial read did not authorize review");
            session.decide(caller,{1,pending.id,pending.revision,pending.target,true});
            decided = true;
        }
        session.advance();
        std::this_thread::yield();
    }
    check(decided && calls == 5 && proof.match() == (matches ? agent::ObservationMatch::satisfied : agent::ObservationMatch::not_satisfied) &&
        request->tool_steps().size() == 3,"real plugin state did not produce the expected proof");
    if (!matches) {
        check(request->operation()->state() == OperationState::pending,"model text completed a mismatched task");
        request->request_cancel(runtime::CancelReason::owner_stop);
        session.advance();
    }
    check(request->operation()->state() == (matches ? OperationState::succeeded : OperationState::cancelled) &&
        request->exit()->state() == OperationState::succeeded && manager.unload("example.status") == plugins::ShutdownResult::closed,
        "verification confused completion or retained native work");
}
}

// 使用固定模型和真实面板验证宿主端读回判定。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 3,"status package and test workspace required");
        verify(argv[1],argv[2],true);
        verify(argv[1],argv[2],false);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
