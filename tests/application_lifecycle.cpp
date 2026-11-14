#include <dafeiyu/plugin_manager.hpp>
#include <dafeiyu/session.hpp>
#include "../plugins/examples/observer/observer_api.h"
#include <fstream>
#include <iostream>
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
}
// 在一个真实宿主进程中完成空插件问答、安装、加载、事件消费、卸载、再次问答和移除。
// 步骤：空插件建立上下文 → 安装加载并在模型等待时消费事件 → 验证失败隔离 → 卸载后继续上下文 → 移除并检查回收。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 3,"observer package and workspace required"); const auto run = workspace(argv[2]);
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
