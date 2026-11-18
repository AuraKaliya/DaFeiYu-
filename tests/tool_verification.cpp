#include "application.hpp"
#include "application_commands.hpp"
#include "chat.hpp"
#include <iostream>
#include <sstream>
#include <thread>

namespace {
using namespace dfy;
using runtime::OperationState;
namespace fs = std::filesystem;
enum class Trial { matches, differs, early_change };

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
model::Response scripted(Trial trial, unsigned call, const model::Request& request) {
    using namespace model;
    if (call == 1) return {{Role::assistant,R"({"schema_version":1,"steps":[{"id":"mode","objective":"Set focus","completion_criterion":"Check actual mode","tools":["example.status/read","example.status/set_focus"]}]})"}};
    check(request.tools.size() == 2,"model did not receive the same two explicit grants");
    if (call == 2) return {{Role::assistant,"",{{"before",request.tools[trial == Trial::early_change ? 1 : 0].name,"{}"}}},FinishReason::tool_calls};
    const auto& last = request.messages.back();
    check(last.role == Role::tool,"tool result was not paired before continuation");
    if (call == 3) return {{Role::assistant,"",{{"change",request.tools[1].name,"{}"}}},FinishReason::tool_calls};
    if (call == 4) return {{Role::assistant,"",{{"after",request.tools[0].name,"{}"}}},FinishReason::tool_calls};
    if (call == 5) {
        check(last.content.find("output:\nfocus") != std::string::npos,"actual state was not read after change");
        return {{Role::assistant,"the requested mode has been set"}};
    }
    throw std::runtime_error("verification invoked the model again");
}

// 经正式终端验证真实面板与模型完成声明的交界。
// 步骤：装配面板及固定响应 → 提交读回任务 → 按基线审批 → 检查实际宿主判定和退出。
void verify(const fs::path& package, const fs::path& root, Trial trial) {
    const auto run = workspace(root);
    unsigned calls = 0;
    app::Application application({},run/"plugins",run/"user",model::Backend{
        [&](const auto&, const auto& request) { return scripted(trial,++calls,request); }});
    auto& manager = application.plugins();
    auto& conversation = application.conversation();
    std::ostringstream output, errors;
    app::ChatController chat(conversation,output,errors,
        [&](std::string_view line) { return app::application_command(application,line,output,errors); });
    manager.install(package);
    check(manager.load("example.status") == 0,"status package did not load");
    chat.line(std::string("/task example.status/read example.status/set_focus ")+(trial == Trial::differs ? "idle" : "focus")+" \"set focus\"");
    const auto accepted = conversation.status();
    chat.line("/verification");
    check(accepted && output.str().find("verification pending.") != std::string::npos && errors.str().empty(),
        "terminal did not show the accepted task and pending evidence");

    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(3);
    bool attempted = false, approved = false;
    while (conversation.busy() && std::chrono::steady_clock::now() < deadline) {
        check(application.advance_plugins() != runtime::RunResult::failed,"native panel advance failed");
        chat.advance();
        const auto confirmations = application.confirmations();
        if (!attempted && !confirmations.empty()) {
            attempted = true;
            const auto& pending = confirmations[0];
            chat.line("/approve "+std::to_string(pending.id)+' '+std::to_string(pending.revision));
            approved = output.str().find("Approval recorded") != std::string::npos;
        }
        std::this_thread::yield();
    }
    const auto report = application.verification();
    const auto status = conversation.status();
    const auto expected = trial == Trial::matches ? agent::ObservationMatch::satisfied :
        (trial == Trial::differs ? agent::ObservationMatch::not_satisfied : agent::ObservationMatch::unknown);
    check(report && report->run.sequence == accepted->turn && report->match == expected && attempted &&
        approved == (trial != Trial::early_change) && calls == (trial == Trial::early_change ? 2u : 5u),
        "real plugin state did not produce the expected proof");
    chat.line("/verification");
    const char* message = trial == Trial::matches ? "matched the actual state." :
        (trial == Trial::differs ? "found a state mismatch." : "could not establish evidence.");
    check(output.str().find(message) != std::string::npos &&
        (trial == Trial::matches) == (output.str().find("the requested mode has been set") != std::string::npos),
        "terminal substituted a model completion for actual evidence");
    check(status && status->result == (trial == Trial::matches ? OperationState::succeeded : OperationState::cancelled) &&
        status->exit == OperationState::succeeded && manager.unload("example.status") == plugins::ShutdownResult::closed,
        "verification confused completion or retained native work");
}
}

// 使用固定模型和真实面板验证宿主端读回判定。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 3,"status package and test workspace required");
        verify(argv[1],argv[2],Trial::matches);
        verify(argv[1],argv[2],Trial::differs);
        verify(argv[1],argv[2],Trial::early_change);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
