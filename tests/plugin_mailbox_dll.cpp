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

// 以同一 C DLL 构建独立的实例，依赖声明及默认配置在包内固定。
void converter(const fs::path& root, const fs::path& dll, std::string_view name, char mode, std::string_view provider = {}) {
    const auto path = root/name;
    fs::create_directories(path);
    fs::copy_file(dll,path/"entry.dll");
    std::ofstream defaults(path/"defaults.json");
    defaults << "{\"mode\":\"" << mode << "\"}";
    std::ofstream manifest(path/"plugin.json");
    manifest << R"({"manifest_version":1,"id":")" << name <<
        R"(","name":"converter","version":"1","abi_version":2,"entry":"entry.dll","default_config":"defaults.json")";
    if (!provider.empty()) manifest << R"(,"dependencies":[")" << provider << R"("])";
    manifest << '}';
}

// 在原加载顺序内验证独立、接续及非法返回的字段改写。
// 步骤：加载提供方及多个改写者 → 校验冲突和作用域 → 替换成依赖接续 → 检查非法长度。
void text_hooks(const fs::path& root, const fs::path& dll) {
    const auto packages = root/"converters";
    converter(packages,dll,"test.a",'a');
    converter(packages,dll,"test.b",'b',"test.a");
    converter(packages,dll,"test.c",'c');
    converter(packages,dll,"test.d",'d');
    converter(packages,dll,"test.z",'z');
    PluginMailboxHost host{std::make_shared<Mailboxes>(MailboxLimits{10,2,20,64}),next_domain()};
    auto channel = std::make_shared<BroadcastChannel>(std::make_shared<EventStore>(8,64),2,2);
    PluginManager manager(packages,root/"converter-data",channel,5,host);
    dfy_v2_text_hook_input event{DFY_V2_TEXT_HOOK_INPUT,0,1,1,0,0,0,1,{"probe",5},{"?",1}};
    check(!manager.load("test.a") && !manager.load("test.d") && manager.rewrite_hook(event,8) == "a","equal independent changes failed");
    check(!manager.load("test.c"),"conflict DLL failed to load");
    bool conflict = false;
    try { (void)manager.rewrite_hook(event,8); }
    catch (const std::runtime_error& issue) { conflict = std::string_view(issue.what()).find("conflicting proposals") != std::string_view::npos; }
    check(conflict,"independent field conflict was silently ordered");
    event.owner = next_domain();
    check(!manager.rewrite_hook(event,8),"field Hook exceeded its session scope");
    event.owner = 0;
    check(manager.unload("test.c") == ShutdownResult::closed && manager.unload("test.d") == ShutdownResult::closed &&
        !manager.load("test.b") && manager.rewrite_hook(event,8) == "b","declared dependency lost preceding change");
    check(!manager.load("test.z"),"invalid result DLL failed to load");
    bool bounded = false;
    try { (void)manager.rewrite_hook(event,8); } catch (const std::length_error&) { bounded = true; }
    check(bounded && manager.close(),"oversized field result or cleanup was not enforced");
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

// 在正式主会话验证各字段实际进入模型、审批与输出，下一轮仍读取原上下文文字。
// 步骤：载入转换插件与业务工具 → 核对改写参数的批准和退出 → 检查回复投递与历史边界。
void run_text_hooks(const fs::path& root, const fs::path& dll, const fs::path& status_package) {
    using namespace dfy::app;
    using namespace dfy::model;
    const auto packages = root/"text-hook-app";
    converter(packages,dll,"test.e",'e');
    std::atomic<unsigned> calls{0};
    Application app({},packages,root/"text-hook-data",Backend{[&](const auto&,const Request& input) {
        if (++calls == 1) {
            check(input.messages.at(0).content == "hook system" && input.messages.back().content == "rewritten prompt",
                "original request did not pass through both text Hooks");
            return Response{{Role::assistant,"",{{"read",input.tools.at(0).name,R"({"bad":1})"}}},FinishReason::tool_calls};
        }
        if (calls == 2) {
            check(input.messages.back().role == Role::tool && input.messages.back().content.find("idle") != std::string::npos,
                "transformed tool did not deliver its original effect");
            return Response{{Role::assistant,"raw"}};
        }
        check(input.messages.at(input.messages.size()-2).content == "model text","delivery Hook rewrote authoritative history");
        return Response{{Role::assistant,"done"}};
    }});
    app.plugins().install(status_package);
    check(!app.plugins().load("test.e") && !app.plugins().load("example.status"),"could not start text Hook fixture");
    // 推进原 Agent 到业务结算和实际退出，保留导致失败的精确异常。
    const auto finish = [&]() {
        std::optional<CompletedTurn> result;
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while (app.conversation().busy() && std::chrono::steady_clock::now() < deadline) {
            if (auto report = app.conversation().advance()) result = std::move(report);
            std::this_thread::yield();
        }
        check(result.has_value() && !app.conversation().busy(),"text Hook turn did not exit");
        if (result->error) std::rethrow_exception(result->error);
        return result->answer;
    };
    app.submit_read_only("example.status/read","hook input");
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while (app.conversation().busy() && app.confirmations().empty() && std::chrono::steady_clock::now() < deadline) {
        app.conversation().advance();
        std::this_thread::yield();
    }
    const auto tickets = app.confirmations();
    check(tickets.size() == 1 && tickets[0].target.arguments == "{}" && calls == 1,"approval did not bind transformed arguments once");
    app.decide(tickets[0].id,tickets[0].revision,true);
    check(finish() == "visible reply","model response and reply delivery did not apply in sequence");
    check(app.plugins().unload("test.e") == ShutdownResult::closed,"idle Hook fixture did not unload");
    app.submit_dialogue("another turn");
    check(finish() == "done" && calls == 3,"unload changed conversation history or retained Hook");
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

// 用真实插件信箱验证被动审查与分类触发共享可信来源。
// 步骤：检查无信号时保留来信 → 主动处理且不启动模型 → 指定分类到达后启动原会话。
void main_inbox(const fs::path& root) {
    using namespace dfy::model;
    using namespace dfy::app;
    std::atomic<unsigned> calls{0};
    Application app({},root,root.parent_path()/"intake-data",Backend{[&](const auto&,const Request& input) {
        ++calls;
        return Response{{Role::assistant,input.messages.back().content}};
    }});
    check(!app.plugins().load("test.mail"),"mail route sender was not loaded");
    const auto instance = app.plugins().instance("test.mail");
    const auto& mail = app.mailbox_host();
    const auto address = instance->mailbox_addresses();
    const auto body = mailbox_text("test.request",1,"source",64);
    // 推进正式调度直到实际 DLL 向本体投递预期回件。
    const auto send = [&](uint64_t sequence) {
        mail.network->deliver(mail.identity,{address.inbound},sequence,body);
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            app.advance_plugins();
            if (mail.network->front(mail.identity,{address.outbound}) || app.conversation().busy()) return;
            std::this_thread::yield();
        }
        throw std::runtime_error("mailbox input did not arrive");
    };
    auto& intake = app.mailbox_intake();
    intake.watch({address.outbound},{},[&](const auto& delivery) -> std::optional<std::string> {
        check(delivery.sender == instance->generation() && delivery.event.id.sequence == 1,"passive review lost identity");
        return {};
    });
    send(1);
    check(!app.conversation().busy() && !calls && intake.review({address.outbound}) == MailboxIntakeResult::handled,
        "passive route incorrectly woke the model");
    const MailboxCategory wake[] = {{"test.reply",1}};
    intake.watch({address.outbound},wake,[](const auto& delivery) -> std::optional<std::string> {
        return std::string(delivery.event.payload->find("text")->as_string());
    });
    send(2);
    check(app.conversation().busy() && app.conversation().status()->source == dfy::agent::RunOrigin::mailbox,
        "configured arrival did not reach the main Agent");
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(3);
    std::optional<CompletedTurn> result;
    while (app.conversation().busy() && std::chrono::steady_clock::now() < deadline) {
        if (auto value = app.conversation().advance()) result = std::move(value);
        std::this_thread::yield();
    }
    check(result && result->answer.find("test.reply") != std::string::npos && result->answer.find("received") != std::string::npos &&
        calls == 1,"main inbox lost its category or payload");
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
        const auto registered = manager.hook_version();
        check(registered.starts_with("hooks-v1:sha256:"),"registered policy lacked a public version");
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

        const auto body = mailbox_text("test.request",1,"source",64);
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
        check(manager.control_hook(control) == DFY_V2_HOOK_ALLOW && manager.hook_version() == "none","unloaded Hook remained active or versioned");
        check(manager.load("test.mail") == DFY_V2_OK && manager.instance("test.mail")->mailbox_addresses().outbound != addresses.outbound,
            "reload reused old mailbox routing");
        check(manager.hook_version() == registered,"instance generation changed the public registration version");
        check(manager.close(),"mailbox manager did not close");
        main_hook(root,argv[3]);
        subagents(root);
        main_inbox(root);
        text_hooks(root.parent_path(),argv[1]);
        run_text_hooks(root.parent_path(),argv[1],argv[3]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
