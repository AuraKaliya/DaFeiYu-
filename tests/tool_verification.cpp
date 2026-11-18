#include "application.hpp"
#include "application_commands.hpp"
#include "chat.hpp"
#include "diagnostic_files.hpp"
#include <iostream>
#include <sstream>
#include <thread>

namespace {
using namespace dfy;
using runtime::OperationState;
namespace fs = std::filesystem;
enum class Trial { matches, differs, early_change, replaced };

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
    // 重新连接的是终端视图，宿主保留原会话和回执。
    const auto connect = [&] {
        return std::make_unique<app::ChatController>(conversation,output,errors,
            [&](std::string_view line) { return app::application_command(application,line,output,errors); });
    };
    auto chat = connect();
    manager.install(package);
    check(manager.load("example.status") == 0,"status package did not load");
    const auto command = std::string("/task example.status/read example.status/set_focus ")+(trial == Trial::differs ? "idle" : "focus")+" \"set focus\"";
    if (trial == Trial::matches) {
        chat->line(command+" --request 2");
        check(!conversation.status() && errors.str().find("sequence gap") != std::string::npos,"task sequence gap admitted an operation");
        errors.str("");
    }
    chat->line(trial == Trial::matches ? command+" --request 1" : command);
    const auto accepted = conversation.status();
    chat->line("/verification");
    check(accepted && output.str().find("verification pending.") != std::string::npos && errors.str().empty(),
        "terminal did not show the accepted task and pending evidence");
    const auto pending_diagnostic = conversation.diagnostic();
    check(pending_diagnostic && pending_diagnostic->timeline.key.run == agent::RunId{accepted->session,accepted->turn} &&
        !agent::review_timeline(pending_diagnostic->timeline).run_exited,"active diagnostic exposed the wrong run state");
    check(pending_diagnostic->timeline.versions.build && !pending_diagnostic->timeline.versions.build->empty() &&
        pending_diagnostic->timeline.versions.prompt_template == "app.verified-state-v1","application did not capture actual build and prompt labels");

    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(3);
    bool attempted = false, approved = false;
    std::string decision_key;
    while (conversation.busy() && std::chrono::steady_clock::now() < deadline) {
        check(application.advance_plugins() != runtime::RunResult::failed,"native panel advance failed");
        chat->advance();
        const auto confirmations = application.confirmations();
        if (!attempted && !confirmations.empty()) {
            attempted = true;
            const auto& pending = confirmations[0];
            if (trial == Trial::replaced)
                check(manager.unload("example.status") == plugins::ShutdownResult::closed && manager.load("example.status") == 0,
                    "could not replace instance while awaiting confirmation");
            if (trial == Trial::matches) {
                chat = connect();
                chat->line("/task-status 1");
                chat->line(command+" --request 1");
                check(output.str().find("waiting=1") != std::string::npos && output.str().find("Original run ") != std::string::npos,
                    "replacement terminal could not find the original pending task");
            }
            decision_key = std::to_string(pending.id)+' '+std::to_string(pending.revision);
            chat->line("/approve "+decision_key);
            approved = output.str().find("Approval recorded") != std::string::npos;
        }
        std::this_thread::yield();
    }
    const auto report = application.verification();
    const auto status = conversation.status();
    const bool has_readback = trial == Trial::matches || trial == Trial::differs;
    const unsigned expected_calls = has_readback ? 5u : trial == Trial::early_change ? 2u : 3u;
    const auto expected = trial == Trial::matches ? agent::ObservationMatch::satisfied :
        (trial == Trial::differs ? agent::ObservationMatch::not_satisfied : agent::ObservationMatch::unknown);
    check(report && report->run.sequence == accepted->turn && report->match == expected && attempted &&
        approved == has_readback && calls == expected_calls,
        "real plugin state did not produce the expected proof");
    chat->line("/verification");
    const char* message = trial == Trial::matches ? "matched the actual state." :
        (trial == Trial::differs ? "found a state mismatch." : "could not establish evidence.");
    check(output.str().find(message) != std::string::npos &&
        (trial == Trial::matches) == (output.str().find("the requested mode has been set") != std::string::npos),
        "terminal substituted a model completion for actual evidence");
    if (trial == Trial::differs)
        check(output.str().find("Request cancelled.\n") != std::string::npos,"terminal reported an exit wait after readback finished");
    const auto diagnostic = conversation.diagnostic();
    check(diagnostic && diagnostic->timeline.key == pending_diagnostic->timeline.key &&
        agent::review_timeline(diagnostic->timeline).run_exited &&
        (!has_readback ? diagnostic->feedback.empty() :
            diagnostic->feedback.size() == 1 && diagnostic->feedback.front().verified == (trial == Trial::matches)),
        "retained diagnostic lost original evidence or run identity");
    const auto path = run/u8"面板诊断.json";
    const auto utf = path.u8string();
    chat->line("/diagnostic export \""+std::string(utf.begin(),utf.end())+"\"");
    const auto stored = app::read_diagnostic_file(path);
    check(stored.timeline.key == diagnostic->timeline.key && stored.feedback.size() == diagnostic->feedback.size() &&
        output.str().find("Diagnostic exported") != std::string::npos && calls == expected_calls,
        "terminal diagnostic export lost evidence or invoked model");
    if (trial == Trial::matches) {
        chat = connect();
        output.str("");
        chat->line("/task-status 1");
        chat->line(command+" --request 1");
        chat->line("/approve "+decision_key);
        chat->line("/deny "+decision_key);
        chat->line("/task example.status/read example.status/set_focus focus \"different goal\" --request 1");
        check(output.str().find("Question=succeeded") != std::string::npos && output.str().find("Original run ") != std::string::npos &&
            output.str().find("Approval recorded") != std::string::npos && errors.str().find("decision receipt conflict") != std::string::npos &&
            errors.str().find("conflicts with receipt") != std::string::npos && calls == 5 && !conversation.busy(),
            "task or decision replay lost its receipt or created a new effect");
    }
    check(status && status->result == (trial == Trial::matches ? OperationState::succeeded : OperationState::cancelled) &&
        status->exit == OperationState::succeeded && manager.unload("example.status") == plugins::ShutdownResult::closed,
        "verification confused completion or retained native work");
}

// 保留窗口满后，旧任务号只能过期，不能变成第二次执行。
// 步骤：逐项接纳并取消 → 核对被换出的旧回执 → 重发旧编号并检查运行未变化。
void retired_receipts(const fs::path& package, const fs::path& root) {
    const auto run = workspace(root);
    app::Application application({},run/"plugins",run/"user",model::Backend{
        [](const auto&,const auto&) { return model::Response{{model::Role::assistant,"unused cancelled response"}}; }});
    application.plugins().install(package);
    check(application.plugins().load("example.status") == 0,"receipt test package did not load");
    auto& conversation = application.conversation();
    for (std::uint64_t request = 1; request <= 17; ++request) {
        application.submit_verified_once(request,"example.status/read","example.status/set_focus","focus","set focus");
        check(conversation.cancel(),"numbered task was not cancellable");
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while (conversation.busy() && std::chrono::steady_clock::now() < deadline) {
            application.advance_plugins();
            conversation.advance();
            std::this_thread::yield();
        }
        check(!conversation.busy() && conversation.status()->result == OperationState::cancelled,
            "cancellation did not release the numbered run");
    }

    check(!application.task_run(1) && application.task_run(2) && application.task_run(17),"receipt retention boundary changed");
    bool expired = false;
    try { application.submit_verified_once(1,"example.status/read","example.status/set_focus","focus","set focus"); }
    catch (const std::invalid_argument& error) { expired = std::string_view(error.what()).find("expired") != std::string_view::npos; }
    check(expired && conversation.status()->turn == 17 && !conversation.busy(),"retired request reexecuted");
}
}

// 使用固定模型和真实面板验证宿主端读回判定。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 3,"status package and test workspace required");
        verify(argv[1],argv[2],Trial::matches);
        verify(argv[1],argv[2],Trial::differs);
        verify(argv[1],argv[2],Trial::early_change);
        verify(argv[1],argv[2],Trial::replaced);
        retired_receipts(argv[1],argv[2]);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
