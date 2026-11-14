#include <dafeiyu/plugin_manager.hpp>
#include <fstream>
#include <iostream>
namespace {
using namespace dfy::plugins;
namespace fs = std::filesystem;
// 在目录管理、实例回收或失败保留违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 创建本轮唯一测试目录，不删除或覆盖既有文件。
fs::path workspace(const fs::path& root) {
    fs::create_directories(root);
    for (unsigned i = 1;; ++i) { const auto path = root/std::to_string(i); if (fs::create_directory(path)) return path; }
}
// 创建零正文事件以确认管理器实际推进了独立观察插件。
std::shared_ptr<const dfy::runtime::StructValue> payload() {
    const auto type = std::make_shared<const dfy::runtime::StructDescriptor>(dfy::runtime::StructDescriptor::create("test.manager",1,{}, {0,32}));
    return std::make_shared<const dfy::runtime::StructValue>(dfy::runtime::StructValue::create(type,{},0));
}
// 验证批量关闭只尝试一次共享依赖，失败不能被同一轮的另一个根重复调用。
// 步骤：建立消费者及提供方 → 固定停止失败 → 记录一次批量关闭的调用增量 → 修复并清理。
void close_once(const fs::path& root, const fs::path& data, const std::shared_ptr<dfy::events::BroadcastChannel>& channel) {
    PluginManager manager(root,data,channel,4);
    check(!manager.load("example.observer") && !manager.load("test.pending"),"batch stop setup failed");
    dfy::platform::DynamicLibrary control(root/"pending"/"entry.dll");
    const auto mode = reinterpret_cast<void (DFY_V2_CALL*)(uint32_t)>(control.symbol("dfy_lifecycle_mode"));
    const auto count = reinterpret_cast<uint32_t (DFY_V2_CALL*)(uint32_t)>(control.symbol("dfy_lifecycle_count"));
    manager.dependencies().bind(manager.instance("test.pending"),manager.instance("example.observer"));
    const auto before = count(2); mode(5); const bool closed = manager.close(); const auto attempts = count(2)-before;
    check(manager.close_status() == ShutdownResult::failed,"batch failure was not reported as current status");
    mode(6); const bool pending = !manager.close() && manager.close_status() == ShutdownResult::pending;
    const auto historical_error = manager.instance("test.pending")->last_error();
    for (unsigned i = 0; i < 8 && !manager.close(); ++i) manager.advance();
    check(manager.close(),"batch stop cleanup failed");
    check(!closed && attempts == 1,"batch close retried the same failed dependency more than once");
    check(pending && historical_error == DFY_V2_FAILED,"historical failure replaced a fresh pending stop result");
    auto failed = std::make_unique<PluginManager>(root,data,channel,4);
    check(!failed->load("test.pending"),"failed manager destructor setup failed");
    const auto old_count = count(2); mode(5); check(!failed->close(),"stop failure unexpectedly completed"); failed.reset();
    const auto destructor_attempts = count(2)-old_count;
    mode(0); check(!PluginManager::retry_retained(),"explicit retained retry failed");
    check(destructor_attempts == 1,"destructor silently retried a reported stop failure");
}
}
// 验证实际包的发现、显式加载、公平消费、卸载、联动等待及管理器失败保留。
// 步骤：准备独立包目录 → 空目录及坏包隔离 → 加载消费并保护忙碌使用 → 验证正常等待及保留重试。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 4,"observer package, lifecycle DLL and workspace required"); const auto run = workspace(argv[3]);
        const auto root = run/"packages", data = run/"data"; fs::create_directories(root);
        auto store = std::make_shared<dfy::events::EventStore>(16,64);
        auto channel = std::make_shared<dfy::events::BroadcastChannel>(store,4,4);
        PluginManager manager(root,data,channel,4); check(manager.catalog()->packages.empty(),"empty directory required a plugin");
        { std::ofstream invalid(run/"root-file"); invalid << "not a directory"; }
        {
            PluginManager unavailable(run/"root-file",data,channel,1);
            check(!unavailable.directory_error().empty() && unavailable.catalog()->packages.empty() && unavailable.close(),"root failure prevented isolated empty-plugin manager");
        }
        fs::copy(fs::canonical(argv[1]),root/"observer",fs::copy_options::recursive);
        fs::create_directory(root/"broken"); { std::ofstream invalid(root/"broken"/"plugin.json"); invalid << "{"; }
        manager.refresh(); check(manager.catalog()->packages.size() == 1 && manager.catalog()->diagnostics.size() == 1,"bad package poisoned catalog");
        check(manager.loaded().empty() && manager.load("example.observer") == DFY_V2_OK && manager.load("example.observer") == DFY_V2_BUSY,
            "refresh loaded automatically or duplicate load succeeded");
        channel->publish(payload());
        for (int i = 0; i < 8; ++i) manager.advance();
        check(!store->references() && channel->size() == 1,"manager did not advance actual observer");
        auto pin = manager.instance("example.observer")->retain_code();
        check(manager.unload("example.observer") == ShutdownResult::in_use && manager.loaded().size() == 1,"manager forced a pinned unload");
        pin.reset(); check(manager.unload("example.observer") == ShutdownResult::closed && manager.loaded().empty() && !channel->size(),"manager unload leaked state");

        const auto pending = root/"pending"; fs::create_directory(pending); fs::copy_file(fs::canonical(argv[2]),pending/"entry.dll");
        { std::ofstream manifest(pending/"plugin.json"); manifest << R"({"manifest_version":1,"id":"test.pending","name":"pending","version":"1","abi_version":2,"entry":"entry.dll"})"; }
        manager.refresh(); check(!manager.load("example.observer") && !manager.load("test.pending"),"linked manager setup failed");
        dfy::platform::DynamicLibrary control(pending/"entry.dll");
        const auto mode = reinterpret_cast<void (DFY_V2_CALL*)(uint32_t)>(control.symbol("dfy_lifecycle_mode"));
        manager.dependencies().bind(manager.instance("test.pending"),manager.instance("example.observer")); mode(4);
        check(manager.unload("example.observer",DependencyStop::linked) == ShutdownResult::pending,"linked pending stop did not return pending");
        manager.advance(); check(manager.loaded().empty(),"manager lost linked stop policy while waiting"); mode(0); control.close();
        check(manager.close(),"empty manager did not close");
        close_once(root,data,channel);

        {
            auto retained = std::make_unique<PluginManager>(root,data,channel,4); check(!retained->load("example.observer"),"retained manager setup failed");
            pin = retained->instance("example.observer")->retain_code(); retained.reset();
        }
        check(PluginManager::retained() == 1 && PluginManager::retry_retained() == 1,"failed manager lost pinned instance");
        pin.reset(); check(!PluginManager::retry_retained() && !channel->size() && PluginInstance::retained().empty(),"retained manager did not finish safely");
        std::cout << "Plugin manager checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
