#include <dafeiyu/plugin_manager.hpp>
#include <dafeiyu/plugin_companion.hpp>
#include <dafeiyu/plugin_tools.hpp>
#include <dafeiyu/tool_executor.hpp>
#include <array>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>

namespace {
using namespace dfy;
using runtime::OperationState;
// 在插件注册或执行行为不符合约定时结束本次探针。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 推进实际执行器，分开等待工具结果与原工作退出。
void until(agent::ToolExecutor& executor, const auto& ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ready() && std::chrono::steady_clock::now() < deadline) {
        executor.advance();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(ready(), "MC tool timeout");
}
// 验证空目录、安装、作用域、管道读回、忙碌卸载和撤销。
// 步骤：安装并加载到真实插件管理器 → 执行两次只读工作 → 撤销目录并卸载。
void verify(const std::filesystem::path& package, const std::filesystem::path& root) {
    auto channel = std::make_shared<events::BroadcastChannel>(std::make_shared<events::EventStore>(16, 64), 4, 8);
    plugins::PluginManager manager(root / "packages", root / "user", channel);
    check(manager.loaded().empty(), "MC plugin auto-loaded");
    auto manifest = manager.install(package);
    check(manifest.id == "mc.companion" && manager.load(manifest.id) == 0, "MC plugin load failed");
    auto catalog = std::make_shared<agent::ToolCatalog>();
    plugins::PluginTools registration(manager.instance(manifest.id), catalog);
    const auto offer = plugins::companion_offer(*manager.instance(manifest.id), *catalog);
    check(offer && offer->state_tool == "mc.companion/world" && offer->state_topic == "world" &&
        offer->generation == manager.instance(manifest.id)->generation(), "MC role not registered by active instance");
    check(plugins::companion_route(*manager.instance(manifest.id), *catalog, "player.inbox", false) == "mc.companion/inbox" &&
        plugins::companion_route(*manager.instance(manifest.id), *catalog, "visit.inspect", true) == "mc.companion/inspect" &&
        !plugins::companion_route(*manager.instance(manifest.id), *catalog, "other", false), "MC purpose registration");
    const std::array<std::string, 2> allowed{"mc.companion/world", "mc.companion/inbox"};
    const auto scope = catalog->snapshot(allowed, false);
    const auto definitions = scope.definitions();
    check(definitions.size() == 2, "MC world or inbox registration missing");
    const agent::RunId run{runtime::next_domain(), 1};
    agent::ToolExecutor executor(run, scope);

    std::string generation;
    for (std::uint64_t step = 1; step <= 3; ++step) {
        const bool inbox = step == 2;
        const auto member = std::find_if(definitions.begin(), definitions.end(), [&](const auto& tool) {
            return tool.name.ends_with(inbox ? "_inbox" : "_world");
        });
        check(member != definitions.end(), "missing registered method");
        const auto args = inbox ? nlohmann::json{{"generation", generation}, {"after", 0}}.dump() : "{}";
        const auto work = executor.submit({{run, step}, 1}, {std::to_string(step), member->name, args});
        until(executor, [&] { return work->operation()->state() != OperationState::pending; });
        check(work->operation()->state() == OperationState::succeeded && work->exit()->state() == OperationState::pending &&
            manager.unload(manifest.id) == plugins::ShutdownResult::in_use, "MC result or busy lifetime incorrect");
        until(executor, [&] { return work->exit()->state() != OperationState::pending; });
        const auto outcome = work->outcome();
        check(outcome.effect == agent::ToolEffect::not_applied, "read changed game");
        const auto value = nlohmann::json::parse(outcome.output);
        if (step == 1) {
            check(value.at("dimension") == "overworld" && value.at("cobblestone") == 5, "wrong MC readback");
            generation = value.at("generation").get<std::string>();
        } else if (inbox) check(value.at("messages").at(0).at("text") == "去山脚看看", "wrong owner message");
        else check(value.is_null(), "revoked world reused");
    }
    registration.revoke();
    bool refused = false;
    try { static_cast<void>(executor.submit({{run, 4}, 1}, {"4", definitions.front().name, "{}"})); }
    catch (const std::invalid_argument&) { refused = true; }
    check(refused && manager.unload(manifest.id) == plugins::ShutdownResult::closed &&
        manager.close(), "revoked MC tool admitted or unload failed");
}
}

// 从包文件和独立测试目录验证真实 DLL。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 3, "expected package and workspace");
        verify(argv[1], argv[2]);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
