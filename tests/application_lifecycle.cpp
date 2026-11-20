#include <dafeiyu/plugin_manager.hpp>
#include "application.hpp"
#include "run_log.hpp"
#include "trace_commands.hpp"
#include "../plugins/examples/observer/observer_api.h"
#include <dafeiyu/run_loop.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
namespace {
using namespace dfy::runtime;
using namespace dfy::plugins;
namespace fs = std::filesystem;
// 在完整业务链、事件消费或包生命周期违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 为完整链路建立唯一目录，不依赖源码根中的插件或用户配置。
fs::path workspace(const fs::path& root) {
    fs::create_directories(root);
    for (unsigned i = 1;; ++i) { const auto path = root/std::to_string(i); if (fs::create_directory(path)) return path; }
}
// 在受控原生调用内查询独立观察包的统计，返回前归还临时代码持有。
dfy_observer_stats statistics(PluginManager& manager) {
    auto code = manager.instance("example.observer")->retain_code(); auto call = code.enter();
    const auto query = reinterpret_cast<dfy_v2_query_fn>(call.symbol(DFY_V2_QUERY_EXPORT));
    const dfy_v2_plugin_api* bootstrap = nullptr; const void* table = nullptr;
    check(query(2,&bootstrap,nullptr) == DFY_V2_OK && bootstrap && bootstrap->query_interface(
        {DFY_OBSERVER_STATS_INTERFACE,sizeof(DFY_OBSERVER_STATS_INTERFACE)-1},1,&table,nullptr) == DFY_V2_OK && table,"integration observer stats missing");
    const auto& api = *static_cast<const dfy_observer_stats_api*>(table);
    check(api.header.struct_size == sizeof(api) && api.header.interface_version == 1 && api.read,"integration observer stats layout wrong");
    dfy_observer_stats result{}; check(api.read(&result,nullptr) == DFY_V2_OK,"integration observer stats failed"); return result;
}
// 持续推进真实宿主及后台模型，观察任务不会阻挡问答的业务完成。
void finish(dfy::agent::Session& session, PluginManager& plugins, const std::shared_ptr<dfy::agent::TurnRequest>& request) {
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (request->exit()->state() == OperationState::pending && std::chrono::steady_clock::now() < deadline) {
        session.advance(); check(plugins.advance() != RunResult::failed,"integration plugin callback failed"); std::this_thread::yield();
    }
    check(request->exit()->state() == OperationState::succeeded,"integration model did not exit");
    for (unsigned i = 0; i < 8; ++i) check(plugins.advance() != RunResult::failed,"integration observer drain failed");
}
struct Release {
    std::promise<void>& promise; bool released = false;
    // 异常退出也唤醒被控制的模型线程，避免测试留下后台等待。
    ~Release() { if (!released) promise.set_value(); }
    // 一次解除正在进行的模型调用。
    void open() { promise.set_value(); released = true; }
};

// 在正式应用装配中验证空插件问答与关闭顺序。
// 步骤：注入固定回答 → 接纳并检查活动时关闭拒绝 → 等待实际退出 → 关闭插件管理。
void shared_application(const fs::path& run) {
    dfy::app::Application application({},run/"shared-plugins",run/"shared-user",dfy::model::Backend{
        [](const auto&, const auto& request) {
            return dfy::model::Response{{dfy::model::Role::assistant,"shared:"+request.messages.back().content}};
        }},dfy::app::ApplicationRunConfig{.trace_directory = run/"direct-trace"});
    auto& conversation = application.conversation();
    const std::string prompt = "hello-"+std::string(20*1024,'a');
    const auto accepted = application.submit_dialogue(prompt);
    bool busy = false;
    try { (void)application.submit_dialogue("another line"); } catch (const std::logic_error&) { busy = true; }
    check(busy,"mailbox changed direct chat busy semantics");
    check(application.close_plugins() == ShutdownResult::busy,"shared application ended plugins before its conversation");

    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    std::optional<dfy::app::CompletedTurn> result;
    while (conversation.busy() && std::chrono::steady_clock::now() < deadline) {
        if (auto latest = conversation.advance()) result = std::move(latest);
        std::this_thread::yield();
    }
    check(result && result->answer == "shared:"+prompt && result->status.turn == accepted.turn &&
        conversation.status()->exit == OperationState::succeeded,"shared application lost result or actual exit");
    std::ostringstream observed, replay;
    const dfy::agent::RunId id{accepted.session,accepted.turn};
    dfy::app::review_trace(application.trace_file(),id,observed);
    dfy::app::replay_trace(application.trace_file(),id,replay);
    check(observed.str().find("\"kind\":\"mail.dispatched\"") != std::string::npos &&
        observed.str().find("\"category\":\"dialogue.text\"") != std::string::npos &&
        replay.str().find("1 model requests, 0 tool exits, succeeded") != std::string::npos,
        "direct mailbox was not linked to its replayed model run");

    conversation.close();
    check(conversation.closed() && application.close_plugins() == ShutdownResult::closed,"shared application could not finish shutdown");
}

// 核对离线运行保留同名工具的原调用顺序和 Hook 实际使用的参数。
// 步骤：记录固定模型及有序改写 → 比对工具回执 → 脱离原 Hook 重新执行。
void trace_rewrites(const fs::path& run) {
    using namespace dfy::agent;
    using namespace dfy::model;
    constexpr const char* name = "read_cell";
    const RunId id{next_domain(),1};
    const auto trace = std::make_shared<dfy::app::RunLog>(run/"rewritten-trace");
    const auto capture = trace->sinks("fixture");
    std::vector<std::string> effective_arguments;
    ToolCatalog catalog;
    const std::vector<std::string> ids{"fixture/cell"};
    catalog.add({ids.front(),next_domain(),1,{name,"Read a cell",
        R"({"type":"object","properties":{"cell":{"type":"integer"}},"required":["cell"],"additionalProperties":false})"}},
        [&](const ToolInvocation& invocation) {
            effective_arguments.push_back(invocation.call.arguments);
            return completed_tool({true,ToolEffect::not_applied,invocation.call.arguments,ToolError::none});
        });
    RunOptions options;
    options.tools = catalog.snapshot(ids);
    options.model_capture = capture.model;
    options.tool_capture = capture.tool;
    options.exit_capture = capture.exit;
    options.text_hook = [](TextHookPoint point,const auto&,auto,std::string_view value,std::size_t) -> std::optional<std::string> {
        if (point == TextHookPoint::model_response) return std::string(value)+" after hook";
        if (point == TextHookPoint::tool_arguments) return value == R"({"cell":1})" ? R"({"cell":11})" : R"({"cell":22})";
        return {};
    };
    RunLoop loop(id,{},Backend{[&](const auto&,const Request& input) {
        if (input.messages.back().role == Role::user)
            return Response{{Role::assistant,"reading",{{"cell-1",name,R"({"cell":1})"},{"cell-2",name,R"({"cell":2})"}}},FinishReason::tool_calls};
        return Response{{Role::assistant,"finished"}};
    }},std::make_shared<MessageHistory>(),"read two cells",std::move(options));

    const auto request = loop.request();
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (request->exit()->state() == OperationState::pending && std::chrono::steady_clock::now() < deadline) {
        loop.advance();
        std::this_thread::yield();
    }
    check(request->exit()->state() == OperationState::succeeded && request->answer() == "finished after hook" &&
        effective_arguments == std::vector<std::string>{R"({"cell":11})",R"({"cell":22})"},"fixture did not use ordered rewrite results");

    std::ostringstream replay;
    dfy::app::replay_trace(trace->path(),id,replay);
    check(replay.str().find("2 model requests, 2 tool exits, succeeded") != std::string::npos,
        "replay lost response rewrite or effective tool arguments");
}

// 验证房间信箱与直接对话独立，自动和主动审查共用分类边界。
// 步骤：投递环境变化及被点名事件 → 验证唤醒模式 → 核对受信来源及模型请求。
void room_mailbox(const fs::path& run, bool automatic) {
    using namespace dfy::app;
    using namespace dfy::model;
    Application application({},run/"room-plugins",run/"room-user",Backend{[](const auto&,const Request& request) {
        return Response{{Role::assistant,request.messages.back().content}};
    }},ApplicationRunConfig{.wake_for_room_address = automatic,.trace_directory = run/"room-trace"});
    check(application.post_room_event(RoomEvent::presence_changed,"room-1","joined").state == dfy::events::DeliveryState::accepted,
        "presence fact did not enter its mailbox");
    application.advance_plugins();
    check(!application.conversation().busy(),"presence fact woke the main Agent");
    check(application.post_room_event(RoomEvent::addressed,"room-1","hello fish").state == dfy::events::DeliveryState::accepted,
        "room address did not queue independently");
    application.advance_plugins();
    application.advance_plugins();
    if (!automatic) {
        check(!application.conversation().busy(),"passive room route woke the Agent");
        check(application.review_room() == MailboxIntakeResult::handled && application.review_room() == MailboxIntakeResult::submitted,
            "review did not filter room presence before direct address");
    }
    check(application.conversation().busy() && application.conversation().status()->source == dfy::agent::RunOrigin::mailbox,
        "direct room address did not reach main Agent");
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    std::optional<CompletedTurn> result;
    while (application.conversation().busy() && std::chrono::steady_clock::now() < deadline) {
        if (auto completion = application.conversation().advance()) result = std::move(completion);
        std::this_thread::yield();
    }
    check(result && result->answer.find("room.addressed") != std::string::npos && result->answer.find("hello fish") != std::string::npos &&
        result->answer.find("joined") == std::string::npos,"room envelope lost its source or leaked a filtered fact");
    std::ostringstream observed;
    review_trace(application.trace_file(),dfy::agent::RunId{result->status.session,result->status.turn},observed);
    check(observed.str().find("\"kind\":\"mail.dispatched\"") != std::string::npos &&
        observed.str().find("room.addressed") != std::string::npos && observed.str().find("joined") == std::string::npos,
        "filtered room presence was incorrectly linked to a model run");
}
}
// 在一个真实宿主进程中完成空插件问答、安装、加载、事件消费、卸载、再次问答和移除。
// 步骤：空插件建立上下文 → 安装加载并在模型等待时消费事件 → 验证失败隔离 → 卸载后继续上下文 → 移除并检查回收。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 3,"observer package and workspace required"); const auto run = workspace(argv[2]);
        shared_application(run);
        trace_rewrites(run);
        room_mailbox(run,true);
        room_mailbox(run,false);
        auto store = std::make_shared<dfy::events::EventStore>(64,64);
        auto channel = std::make_shared<dfy::events::BroadcastChannel>(store,4,8);
        PluginManager manager(run/"plugins",run/"user",channel,4);
        std::promise<void> release, entered; const auto gate = release.get_future().share(); auto started = entered.get_future();
        std::vector<dfy::model::Message> messages;
        dfy::agent::Session session({},channel,4,
        // 复制真实上下文并按测试回合选择等待、失败或成功，不接触宿主操作对象。
        // 步骤：保存输入 → 按提示等待或失败 → 返回完整回答。
        [&](const auto&,const auto& input) {
            messages = input;
            if (input.back().content == "with plugin") { entered.set_value(); gate.wait(); }
            if (input.back().content == "failure") throw std::runtime_error("model: integration failure");
            return "answer:"+input.back().content;
        });
        Release cleanup{release}; auto baseline = session.begin("baseline"); finish(session,manager,baseline);
        check(baseline->answer() == "answer:baseline" && manager.loaded().empty() && session.message_count() == 2,"empty-plugin answer failed");
        manager.install(argv[1]); check(manager.loaded().empty() && !manager.load("example.observer"),"install or explicit load failed");
        auto request = session.begin("with plugin"); started.wait();
        for (unsigned i = 0; i < 8; ++i) manager.advance();
        check(request->operation()->state() == OperationState::pending && statistics(manager).events == 1,"model wait blocked actual DLL event consumption");
        cleanup.open(); finish(session,manager,request);
        check(request->answer() == "answer:with plugin" && statistics(manager).events == 2 && !statistics(manager).failures &&
            messages.size() == 3 && messages[0].content == "baseline" && session.message_count() == 4,"loaded observer changed model context or result");
        auto failure = session.begin("failure"); finish(session,manager,failure);
        check(failure->operation()->state() == OperationState::failed && session.message_count() == 4 && statistics(manager).events == 4,
            "model failure polluted context or stopped observer");
        check(manager.unload("example.observer") == ShutdownResult::closed && manager.loaded().empty(),"integration observer did not unload");
        baseline.reset(); request.reset(); failure.reset(); auto after = session.begin("after unload"); finish(session,manager,after);
        check(after->answer() == "answer:after unload" && messages.size() == 5 && messages[4].content == "after unload" && session.message_count() == 6,
            "unloaded plugin became a model dependency");
        const auto removed = manager.remove("example.observer");
        check(removed.files.residual.empty() && manager.catalog()->packages.empty() && !store->references() && !channel->size(),"integration removal leaked package or event references");
        session.close(); check(session.closed() && manager.close() && !PluginManager::retained() && PluginInstance::retained().empty(),"integration left retained cleanup");
        std::cout << "Application lifecycle checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
