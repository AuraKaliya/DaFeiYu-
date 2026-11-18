#include <dafeiyu/plugin_manager.hpp>
#include <dafeiyu/plugin_tools.hpp>
#include <dafeiyu/tool_executor.hpp>
#include <array>
#include <iostream>
#include <thread>

namespace {
using namespace dfy;
using runtime::OperationState;
namespace fs = std::filesystem;

// 在实际面板状态、效果或资源寿命违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

// 建立独占测试目录供真实包安装使用。
fs::path workspace(const fs::path& root) {
    fs::create_directories(root);
    for (unsigned i = 1;; ++i) {
        const auto path = root/std::to_string(i);
        if (fs::create_directory(path)) return path;
    }
}

// 按宿主节拍推进到预期的结果或真实退出边界。
void until(agent::ToolExecutor& executor, const auto& ready) {
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (!ready() && std::chrono::steady_clock::now() < deadline) { executor.advance(); std::this_thread::yield(); }
    check(ready(),"sample tool did not finish requested phase");
}

// 安装、加载并使用发布形态的状态面板。
// 步骤：注册实际 DLL → 校验读结果和忙碌保护 → 验证变更及幂等回执 → 退出后卸载。
void panel(const fs::path& package, const fs::path& root) {
    using agent::ToolEffect;
    const auto run = workspace(root);
    auto channel = std::make_shared<events::BroadcastChannel>(std::make_shared<events::EventStore>(16,64),4,8);
    plugins::PluginManager manager(run/"plugins",run/"user",channel);
    const auto manifest = manager.install(package);
    check(manifest.id == "example.status" && manager.loaded().empty() && manager.load(manifest.id) == 0,
        "sample package was not explicitly loadable");
    auto catalog = std::make_shared<agent::ToolCatalog>();
    plugins::PluginTools registration(manager.instance(manifest.id),catalog);
    const std::array<std::string,3> ids{"example.status/read","example.status/set_idle","example.status/set_focus"};
    const auto scope = catalog->snapshot(ids,true);
    const auto definitions = scope.definitions();
    check(definitions.size() == ids.size(),"sample tools missing");

    const agent::RunId id{runtime::next_domain(),1};
    agent::ToolExecutor executor(id,scope);
    // 执行已授权的工具并检查结果与实际退出之间的卸载边界。
    // 步骤：提交具体工具 → 检查结果已出现时仍拒绝卸载 → 等待释放并返回实际回执。
    const auto execute = [&](std::size_t index, std::uint64_t step) {
        const auto request = executor.submit({{id,step},1},{"state",definitions[index].name,"{ }"});
        until(executor,[&] { return request->operation()->state() != OperationState::pending; });
        check(request->exit()->state() == OperationState::pending &&
            manager.unload(manifest.id) == plugins::ShutdownResult::in_use &&
            manager.instance(manifest.id)->phase() == plugins::InstancePhase::active,
            "native work did not retain its instance before actual exit");
        until(executor,[&] { return request->exit()->state() != OperationState::pending; });
        check(request->operation()->state() == OperationState::succeeded,"panel operation failed");
        return request->outcome();
    };
    const auto initial = execute(0,1);
    const auto changed = execute(2,2);
    const auto read = execute(0,3);
    const auto repeated = execute(2,4);
    check(initial.output == "idle" && initial.effect == ToolEffect::not_applied &&
        changed.output == "focus" && changed.effect == ToolEffect::applied &&
        read.output == "focus" && read.effect == ToolEffect::not_applied && repeated.effect == ToolEffect::not_applied,
        "read, real effect or repeated assignment facts were wrong");
    check(execute(1,5).output == "idle" && manager.unload(manifest.id) == plugins::ShutdownResult::closed && manager.close(),
        "sample could not reverse mode and release its instance");
}
}

// 运行独立状态面板的原生检验，入口不包含模型。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 3,"sample package and test workspace required");
        panel(argv[1],argv[2]);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
