#include <dafeiyu/plugin_manager.hpp>
#include <dafeiyu/plugin_tools.hpp>
#include <dafeiyu/tool_executor.hpp>
#include <array>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>

namespace {
using namespace dfy;
using runtime::OperationState;
// 检查移动和并行读回是否保持各自的结果与实际退出。
void check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
}
// 从真实包执行玩家原指令，并在实际寻路期间并发读取世界。
// 步骤：登记两个工具 → 开始查看并并行重读 → 核验到达、可见圆石及实际卸载。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 4, "package, test root and generation required");
        const std::filesystem::path root = argv[2];
        const std::string generation = std::filesystem::path(argv[3]).string();
        auto events = std::make_shared<events::BroadcastChannel>(std::make_shared<events::EventStore>(16, 64), 4, 8);
        plugins::PluginManager manager(root / "packages", root / "user", events);
        manager.install(argv[1]);
        check(manager.load("mc.companion") == 0, "MC load");
        auto catalog = std::make_shared<agent::ToolCatalog>();
        plugins::PluginTools registration(manager.instance("mc.companion"), catalog);
        const std::array<std::string, 1> visit_id{"mc.companion/inspect"}, read_id{"mc.companion/world"};
        const auto visit_scope = catalog->snapshot(visit_id, true), read_scope = catalog->snapshot(read_id, false);
        const agent::RunId moving_id{runtime::next_domain(), 1}, query_id{runtime::next_domain(), 1};
        agent::ToolExecutor moving(moving_id, visit_scope), reader(query_id, read_scope);
        const auto args = nlohmann::json{{"generation", generation}, {"order", 1}, {"place", "hillside"}}.dump();
        const auto action = moving.submit({{moving_id, 1}, 1}, {"look", visit_scope.definitions().front().name, args});
        const auto start = std::chrono::steady_clock::now();
        std::shared_ptr<agent::ToolRequest> read;
        bool read_while_moving = false;
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(22)) {
            moving.advance();
            if (!read && std::chrono::steady_clock::now() - start > std::chrono::milliseconds(300))
                read = reader.submit({{query_id, 1}, 1}, {"position", read_scope.definitions().front().name, "{}"});
            reader.advance();
            if (read && read->operation()->state() == OperationState::succeeded && action->operation()->state() == OperationState::pending)
                read_while_moving = true;
            if (read && read->exit()->state() != OperationState::pending && action->exit()->state() != OperationState::pending) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(read_while_moving && action->operation()->state() == OperationState::succeeded &&
            action->exit()->state() == OperationState::succeeded && action->outcome().effect == agent::ToolEffect::applied, "native visit incomplete");
        const auto result = nlohmann::json::parse(action->outcome().output);
        check(result.at("phase") == "arrived" && result.at("generation") == generation && result.at("stone").at("x") == 15,
            "wrong arrival evidence");
        std::cout << result.dump() << '\n';
        registration.revoke();
        check(manager.unload("mc.companion") == plugins::ShutdownResult::closed && manager.close(), "MC unload");
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
