#include "../decision_input.hpp"
#include "application.hpp"
#include "application_commands.hpp"
#include "chat.hpp"
#include <dafeiyu/tool_abi_v2.h>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <thread>

namespace {
using namespace dfy;
namespace fs = std::filesystem;

// 在应用工具授权或运行事实违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

// 创建由此次测试独占的插件和用户目录。
fs::path workspace(const fs::path& root) {
    fs::create_directories(root);
    for (unsigned i = 1;; ++i) {
        const auto path = root/std::to_string(i);
        if (fs::create_directory(path)) return path;
    }
}

// 检查非法工具授予在模型接纳前被拒绝。
void rejects(app::Application& application, const std::string& tool) {
    try { application.submit_read_only(tool,"read"); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("application accepted unavailable tool");
}

// 推进原应用直至业务结果及实际退出均可见。
agent::TurnDiagnostics finish(app::Application& application, app::ChatController& chat) {
    auto& conversation = application.conversation();
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    do {
        check(application.advance_plugins() != runtime::RunResult::failed,"application plugin advance failed");
        chat.advance();
        std::this_thread::yield();
    } while (conversation.busy() && std::chrono::steady_clock::now() < deadline);
    const auto status = conversation.status();
    check(status && status->result == runtime::OperationState::succeeded && status->exit == runtime::OperationState::succeeded,
        "terminal tool run did not finish");
    return *status;
}

// 使用正式应用、真实只读 DLL 和固定结构化响应验证一次工具运行。
// 步骤：拒绝未加载及写入授予 → 给模型一个只读定义 → 验证配对结果 → 卸载后继续普通问答。
void read_only(const fs::path& package, const fs::path& root) {
    using namespace model;
    const auto run = workspace(root);
    unsigned calls = 0;
    app::Application application({},run/"plugins",run/"user",Backend{
        // 仅提供本轮读调用、配对回复和后续普通问答三个结果。
        // 步骤：核对唯一工具范围 → 验证原工具结果及身份 → 验证后续问答只保留历史。
        [&](const Config&, const Request& request) {
            const auto& last = dfy::test::business_messages(request).back();
            switch (++calls) {
            case 1:
                check(last.content == "read the value" && request.tools.size() == 1 && request.tools[0].name.ends_with("_read"),
                    "application exposed a different tool scope");
                return Response{{Role::assistant,"",{{"read-1",request.tools[0].name,"{}"}}},FinishReason::tool_calls};
            case 2:
                check(last.role == Role::tool && last.tool_call_id == "read-1" && last.content.find("output:\n0") != std::string::npos,
                    "native read result was not paired");
                return Response{{Role::assistant,"read verified"}};
            case 3:
                check(request.tools.empty() && last.content == "plain" && std::ranges::any_of(request.messages,[](const auto& previous) {
                    return previous.role==Role::tool && previous.tool_call_id=="read-1" && previous.content.find("output:\n0")!=std::string::npos;
                }),
                    "plain turn lost structured history or inherited tool grant");
                return Response{{Role::assistant,"plain reply"}};
            }
            throw std::runtime_error("unexpected application model call");
        }});
    auto& manager = application.plugins();
    manager.install(package);
    rejects(application,"test.tools/read");
    check(manager.load("test.tools") == 0,"tool fixture did not load");
    rejects(application,"test.tools/increment");
    rejects(application,"test.tools/missing");
    std::ostringstream output, errors;
    app::ChatController chat(application.conversation(),output,errors,
        [&](std::string_view line) { return app::application_command(application,line,output,errors); });
    chat.line("/run test.tools/read");
    chat.line("/run test.tools/increment \"read the value\"");
    check(!application.conversation().status() && calls == 0 && errors.str().find("Usage: /run") != std::string::npos &&
        errors.str().find("not authorized") != std::string::npos,"rejected terminal grant admitted a run or hid its error");

    errors.str("");
    chat.line("/run test.tools/read \"read the value\"");
    const auto admitted = application.conversation().status();
    const auto result = finish(application,chat);
    check(admitted && result.turn == admitted->turn && result.tool_requests == 1 && errors.str().empty() &&
        output.str().find("Accepted run ") != std::string::npos && output.str().find("read verified") != std::string::npos,
        "terminal read-only run lost its receipt or answer");
    check(manager.unload("test.tools") == plugins::ShutdownResult::closed,"run scope pinned exited instance");
    chat.line("plain");
    check(finish(application,chat).tool_requests == 0 && output.str().find("plain reply") != std::string::npos &&
        calls == 3,"ordinary question failed after tool use");
    chat.request_exit();
    check(chat.done() && application.close_plugins() == plugins::ShutdownResult::closed,"application tool test did not close");
}

// 只读复制测试 DLL 的真实效果计数，返回前释放临时代码持有。
std::uint32_t effects(plugins::PluginManager& manager) {
    auto code = manager.instance("test.tools")->retain_code();
    auto call = code.enter();
    const auto count = reinterpret_cast<std::uint32_t (DFY_V2_CALL*)(std::uint32_t)>(call.symbol("dfy_tool_test_count"));
    return count(1);
}

// 用原生非幂等工具验证终端确认及 EOF 停止。
// 步骤：接纳并显示待审动作 → 核对零效果 → 按决定或 EOF 推进 → 检查实际作用次数。
void confirmation(const fs::path& package, const fs::path& root, std::optional<bool> approve) {
    using namespace model;
    const auto run = workspace(root);
    app::Application application({},run/"plugins",run/"user",Backend{
        // 从固定模型返回候选，并核对应用授权后的配对事实。
        // 步骤：为本轮范围提出一次动作 → 分别验证执行或拒绝 → 返回预设说明。
        [approve](const Config&, const Request& request) {
            const auto& last = dfy::test::business_messages(request).back();
            if (last.role == Role::user) {
                check(request.tools.size() == 1 && request.tools[0].name.ends_with("_increment"),"wrong write scope");
                return Response{{Role::assistant,"",{{"write-1",request.tools[0].name,"{}"}}},FinishReason::tool_calls};
            }
            check(last.role == Role::tool && last.tool_call_id == "write-1" &&
                approve && last.content.find(*approve ? "output:\n1" : "status=rejected") != std::string::npos,
                "decision lost the original tool result");
            return Response{{Role::assistant,"decision recorded"}};
        }});
    auto& manager = application.plugins();
    manager.install(package);
    check(manager.load("test.tools") == 0,"confirmation fixture did not load");
    const auto before = effects(manager);
    std::ostringstream output, errors;
    app::ChatController chat(application.conversation(),output,errors,
        [&](std::string_view line) { return app::application_command(application,line,output,errors); });
    chat.line("/run-confirmed test.tools/increment change");

    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    auto values = application.confirmations();
    while (values.empty() && std::chrono::steady_clock::now() < deadline) {
        application.advance_plugins();
        chat.advance();
        values = application.confirmations();
        std::this_thread::yield();
    }
    check(values.size() == 1 && effects(manager) == before && values[0].state == agent::ConfirmationState::waiting &&
        values[0].target.arguments == "{}","application ran a state change before confirmation");
    const auto& pending = values[0];
    chat.line("/confirmations");
    check(output.str().find("test.tools/increment {}") != std::string::npos &&
        output.str().find("Confirmation "+std::to_string(pending.id)+" revision "+std::to_string(pending.revision)) != std::string::npos,
        "terminal did not show the exact pending action");

    if (!approve) {
        chat.finish_input();
        const auto close_deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while (!chat.done() && std::chrono::steady_clock::now() < close_deadline) { application.advance_plugins(); chat.advance(); }
        check(chat.done() && effects(manager) == before && application.conversation().status()->result == runtime::OperationState::cancelled &&
            application.close_plugins() == plugins::ShutdownResult::closed,"EOF waited indefinitely or applied an unapproved action");
        return;
    }

    const auto command = std::string(*approve ? "/approve " : "/deny ")+std::to_string(pending.id)+' ';
    chat.line(command+std::to_string(pending.revision+1));
    check(errors.str().find("stale decision") != std::string::npos,"terminal accepted an incorrect decision revision");
    errors.str("");
    output.str("");
    chat.line(command+std::to_string(pending.revision));
    const auto receipt = output.str();
    output.str("");
    chat.line(command+std::to_string(pending.revision));
    check(output.str() == receipt && receipt.find(*approve ? "Approval recorded" : "Refusal recorded") != std::string::npos &&
        effects(manager) == before,"terminal decision replay changed the original receipt or executed inline");
    check(finish(application,chat).confirmation_requests == 1 && effects(manager) == before+(*approve ? 1u : 0u) &&
        errors.str().empty(),"approval or refusal applied an incorrect number of real effects");
}
}

// 验证共享应用中的只读工具装配，所有模型响应都来自替身。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 3,"tool fixture and test workspace required");
        read_only(argv[1],argv[2]);
        confirmation(argv[1],argv[2],true);
        confirmation(argv[1],argv[2],false);
        confirmation(argv[1],argv[2],std::nullopt);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
