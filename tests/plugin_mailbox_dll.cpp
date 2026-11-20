#include <dafeiyu/plugin_manager.hpp>
#include "fixtures/query_v2/mailbox_fixture.h"
#include "application.hpp"
#include <fstream>
#include <iostream>
#include <atomic>
#include <thread>

namespace {
using namespace dfy::events;
using namespace dfy::plugins;
using namespace dfy::runtime;
namespace fs = std::filesystem;

// 报告跨 DLL 读写和实例销毁的具体失败点。
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

// 构建本次独立测试包，保留其他运行的目录。
fs::path package(const fs::path& root, const fs::path& dll) {
    fs::create_directories(root);
    for (unsigned index = 1;; ++index) {
        const auto path = root/std::to_string(index)/"plugins"/"mail";
        if (!fs::create_directories(path)) continue;
        fs::copy_file(dll,path/"entry.dll");
        std::ofstream manifest(path/"plugin.json");
        manifest << R"({"manifest_version":1,"id":"test.mail","name":"mailbox example","version":"1","abi_version":2,"entry":"entry.dll"})";
        return path.parent_path();
    }
}

// 通过正式应用验证 Hook 可以拒绝主回合并给原生工具加审批。
// 步骤：装配固定后端与原生包 → 批准原候选并核对实效 → 拒绝新请求 → 卸载后恢复普通问答。
void main_hook(const fs::path& root, const fs::path& status_package) {
    using namespace dfy::model;
    std::atomic<unsigned> calls{0};
    dfy::app::Application app({},root,root.parent_path()/"app-data",Backend{
        [&](const auto&,const Request& request) {
            ++calls;
            if (request.messages.back().content == "read mode")
                return Response{{Role::assistant,"",{{"read",request.tools.at(0).name,"{}"}}},FinishReason::tool_calls};
            return Response{{Role::assistant,request.messages.back().role == Role::tool ? request.messages.back().content : "allowed"}};
        }});
    app.plugins().install(status_package);
    check(!app.plugins().load("test.mail") && !app.plugins().load("example.status"),"Hook application setup failed");
    // 推进原会话直到等待审批或报告结算并实际退出。
    const auto advance = [&](bool approval) {
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(3);
        std::optional<dfy::app::CompletedTurn> result;
        while (std::chrono::steady_clock::now() < deadline) {
            if (approval && !app.confirmations().empty()) return dfy::app::CompletedTurn{};
            if (auto polled = app.conversation().advance()) result = std::move(polled);
            if (result && !app.conversation().busy()) return *result;
            std::this_thread::yield();
        }
        throw std::runtime_error("Hook application did not reach expected boundary");
    };
    app.submit_read_only("example.status/read","read mode");
    advance(true);
    const auto waiting = app.confirmations();
    check(waiting.size() == 1 && waiting[0].state == dfy::agent::ConfirmationState::waiting && calls == 1,"tool Hook did not wait for approval");
    app.decide(waiting[0].id,waiting[0].revision,true);
    check(advance(false).answer.find("idle") != std::string::npos && calls == 2,"approved Hook candidate did not execute original tool");
    bool blocked = false;
    try { app.conversation().submit("blocked"); }
    catch (const std::runtime_error& error) { blocked = std::string_view(error.what()) == "run: model request denied"; }
    check(blocked && calls == 2,"deny Hook did not prevent model dispatch");
    check(app.plugins().unload("test.mail") == ShutdownResult::closed,"control Hook held an idle plugin");
    app.conversation().submit("hello");
    check(advance(false).answer == "allowed" && calls == 3,"unloaded Hook continued to deny");
    app.conversation().close();
    check(app.close_plugins() == ShutdownResult::closed,"Hook application did not release resources");
}

// 验证同一插件的子会话没有共享历史，且提交与模型派发分离。
// 步骤：建立两个会话及释放前取消 → 推进固定回答 → 分别核对历史与身份。
void subagents(const fs::path& root) {
    using namespace dfy::model;
    using namespace dfy::app;
    std::atomic<unsigned> calls{0};
    Application app({},root,root.parent_path()/"subagent-data",Backend{[&](const auto&,const Request& request) {
        ++calls;
        return Response{{Role::assistant,std::to_string(request.messages.size())}};
    }});
    check(!app.plugins().load("test.mail"),"could not start subagent owner");
    const auto instance = app.plugins().instance("test.mail");
    auto& agents = app.subagents();
    const auto owner = instance->generation();
    const auto a = agents.open(instance), b = agents.open(instance);
    check(agents.submit(owner,a,{"first"}) == 1 && agents.busy(owner) && calls == 0,"queued turn dispatched inline");
    check(agents.submit(owner,b,{"cancelled"}) == 1 && agents.cancel(owner,b,1),"queued cancellation failed");
    bool denied = false;
    try { (void)agents.status(next_domain(),a); } catch (const std::invalid_argument&) { denied = true; }
    check(denied,"another owner read an isolated session");
    // 等待当前回合实际退出并返回值结果。
    const auto settle = [&](auto session) {
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            app.advance_plugins();
            const auto state = agents.status(owner,session);
            if (state.phase == SubagentPhase::succeeded) return std::string(state.answer);
            std::this_thread::yield();
        }
        throw std::runtime_error("subagent did not settle");
    };
    check(settle(a) == "1" && calls == 1,"first session or queued cancellation was incorrect");
    check(agents.submit(owner,a,{"second"}) == 2 && settle(a) == "3","child context did not persist");
    check(agents.submit(owner,b,{"independent"}) == 2 && settle(b) == "1","child context crossed sessions");
    {
        const auto controller = instance->query<dfy_test_mailbox_api>(DFY_TEST_MAILBOX_INTERFACE,1);
        std::uint64_t request = 0;
        check(controller && instance->invoke(*controller,[&](const auto& api, auto* plugin) {
            return api.ask(plugin,&request,nullptr);
        }) == DFY_V2_OK && request == 1 && calls == 3,"native ABI dispatched before leaving plugin callback");
    }
    check(app.plugins().unload("test.mail") == ShutdownResult::busy,"queued native subagent allowed unload");
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(3);
    do { app.advance_plugins(); std::this_thread::yield(); }
    while (agents.busy(owner) && std::chrono::steady_clock::now() < deadline);
    {
        const auto controller = instance->query<dfy_test_mailbox_api>(DFY_TEST_MAILBOX_INTERFACE,1);
        check(controller && instance->invoke(*controller,[](const auto& api, auto* plugin) -> uint32_t {
            dfy_v2_agent_status status{};
            return !api.agent_result(plugin,&status,nullptr) && status.phase == DFY_V2_AGENT_SUCCEEDED &&
                std::string_view(status.answer.data,status.answer.length) == "1" ? DFY_V2_OK : DFY_V2_FAILED;
        }) == DFY_V2_OK,"native subagent did not yield an isolated result");
    }
    app.conversation().close();
    check(app.close_plugins() == ShutdownResult::pending,"shutdown did not account for child sessions");
    app.advance_plugins();
    check(agents.empty() && app.close_plugins() == ShutdownResult::closed,"child sessions did not release before shutdown");
}
}

// 验证从正式插件管理器载入的 C DLL 可以收到本体请求并回信。
// 步骤：加载并定位信箱 → 投递与回信 → 检查实际分类 → 卸载并阻止旧地址复用。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 4,"mailbox DLL, workspace and status package required");
        const auto root = package(argv[2],argv[1]);
        PluginMailboxHost host{std::make_shared<Mailboxes>(MailboxLimits{2,2,4,64}),next_domain()};
        auto channel = std::make_shared<BroadcastChannel>(std::make_shared<EventStore>(8,64),2,2);
        PluginManager manager(root,root.parent_path()/"data",channel,1,host);
        check(manager.load("test.mail") == DFY_V2_OK,"mailbox plugin failed to attach at load");
        const auto instance = manager.instance("test.mail");
        const auto addresses = instance->mailbox_addresses();
        check(addresses.inbound && addresses.outbound,"manager lost registered mailbox addresses");
        const auto hooks = instance->hooks();
        const dfy_v2_hook_input control{DFY_V2_HOOK_TOOL_START,0,1,1,1,1,0,1,{}};
        check(hooks.size() == 2 && hooks[0].point == DFY_V2_HOOK_MODEL_REQUEST &&
            hooks[1].sessions == (DFY_V2_HOOK_MAIN_SESSION|DFY_V2_HOOK_OWN_SESSION) &&
            instance->decide_hook(1,control) == DFY_V2_HOOK_CONFIRM,"instance lost its C Hook registration");
        auto scoped = control;
        check(manager.control_hook(scoped) == DFY_V2_HOOK_CONFIRM,"main session skipped its control Hook");
        scoped.owner = instance->generation();
        check(manager.control_hook(scoped) == DFY_V2_HOOK_CONFIRM,"owned session skipped its control Hook");
        scoped.owner = next_domain();
        check(manager.control_hook(scoped) == DFY_V2_HOOK_ALLOW,"Hook exceeded its registered scope");

        auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.request",1,{}, {0,32}));
        auto body = std::make_shared<const StructValue>(StructValue::create(std::move(type),{},0));
        check(host.network->deliver(host.identity,{addresses.inbound},1,body).state == DeliveryState::accepted,
            "host request was not admitted");
        {
            const auto reply = instance->query<dfy_test_mailbox_api>(DFY_TEST_MAILBOX_INTERFACE,1);
            check(reply && instance->invoke(*reply,[](const auto& api, auto* plugin) { return api.reply(plugin,nullptr); }) == DFY_V2_OK,
                "actual C plugin could not poll and post");
        }
        auto answer = host.network->read(host.identity,{addresses.outbound});
        check(answer.event.get().payload->descriptor()->name() == "test.reply" &&
            answer.event.get().payload->find("text")->as_string() == "received","host received invalid reply");
        check(host.network->deliver(host.identity,{addresses.inbound},2,body).state == DeliveryState::accepted,
            "activity request was not admitted");
        check(manager.advance() == RunResult::executed && host.network->read(host.identity,{addresses.outbound}).event.get().id.sequence == 2,
            "scheduler did not advance the plugin's independent loop");
        check(manager.unload("test.mail") == ShutdownResult::closed && !instance->mailbox_addresses().inbound,"mailbox blocked clean unload");
        check(manager.control_hook(control) == DFY_V2_HOOK_ALLOW,"unloaded Hook was used");
        check(manager.load("test.mail") == DFY_V2_OK && manager.instance("test.mail")->mailbox_addresses().outbound != addresses.outbound,
            "reload reused old mailbox routing");
        check(manager.close(),"mailbox manager did not close");
        main_hook(root,argv[3]);
        subagents(root);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
