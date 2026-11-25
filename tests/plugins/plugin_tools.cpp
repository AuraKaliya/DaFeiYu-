#include <dafeiyu/plugin_tools.hpp>
#include <dafeiyu/tool_executor.hpp>
#include <iostream>
#include <fstream>

namespace {
using namespace dfy::plugins;
using namespace dfy::agent;
// 报告真实插件注册或生命周期错误。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 确认错误描述、授权或过期快照被拒绝。
void rejects(const auto& action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid native tool registration accepted");
}
// 复制旧无工具能力 DLL 到独立包，验证可选接口不会变成必需依赖。
// 步骤：创建新目录 → 写入旧包描述并复制 DLL → 读取正式清单。
Manifest legacy_package(const std::filesystem::path& dll, const std::filesystem::path& root) {
    namespace fs = std::filesystem; fs::create_directories(root); fs::path run;
    for (unsigned i = 1;; ++i) { run = root/std::to_string(i); if (fs::create_directory(run)) break; }
    fs::copy_file(dll,run/"entry.dll"); std::ofstream file(run/"plugin.json");
    file << R"({"manifest_version":1,"id":"legacy.tools","name":"legacy","version":"1","abi_version":2,"entry":"entry.dll"})";
    file.close(); check(static_cast<bool>(file),"legacy manifest write failed"); return load_manifest(run);
}
}
// 使用真实 C DLL 验证原子注册、命名空间、撤销及同包新实例隔离。
// 步骤：拒绝非法和超量发布 → 完整绑定描述 → 验证在途撤销 → 重新创建实例并拒绝旧快照。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 4,"expected package, settings and legacy DLL"); const auto manifest = load_manifest(argv[1]);
        dfy::platform::DynamicLibrary control(manifest.entry);
        const auto mode = reinterpret_cast<void (DFY_V2_CALL*)(uint32_t)>(control.symbol("dfy_tool_test_mode"));
        auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,0),2,2);
        auto instance = std::make_shared<PluginInstance>(manifest,argv[2],channel);
        check(!instance->create() && !instance->start(),"instance start failed");
        const std::vector<std::string> read_id{"test.tools/read"};
        auto catalog = std::make_shared<ToolCatalog>(4);
        mode(4); rejects([&] { PluginTools invalid(instance,catalog); }); mode(0);
        rejects([&] { (void)catalog->snapshot(read_id); });
        auto small = std::make_shared<ToolCatalog>(1); rejects([&] { PluginTools invalid(instance,small); });
        rejects([&] { (void)small->snapshot(read_id); });
        PluginTools binding(instance,catalog); const auto ids = binding.ids();
        check(ids.size() == 2 && ids[0] == "test.tools/read" && ids[1] == "test.tools/increment","package namespace lost");
        rejects([&] { (void)catalog->snapshot(ids); });
        const auto snapshot = catalog->snapshot(ids,true); const auto definitions = snapshot.definitions();
        check(definitions.size() == 2 && definitions[1].name.size() <= 64 && definitions[1].name.ends_with("_increment"),"model name mapping invalid");
        const AttemptId attempt{StepId{RunId{1,1},1},1};
        auto driver = snapshot.prepare(attempt,{"one",definitions[1].name,"{}"}); driver->advance();
        binding.revoke(); rejects([&] { (void)snapshot.prepare(attempt,{"two",definitions[1].name,"{}"}); });
        check(snapshot.definitions().empty() && instance->shutdown() == ShutdownResult::in_use,"revocation released in-flight code");
        driver->advance(); check(driver->request()->outcome().output == "1" && instance->shutdown() == ShutdownResult::closed,
            "revoked in-flight work did not complete through original instance");
        auto replacement = std::make_shared<PluginInstance>(manifest,argv[2],channel);
        check(!replacement->create() && !replacement->start(),"replacement start failed");
        PluginTools rebound(replacement,catalog); const auto fresh = catalog->snapshot(rebound.ids(),true);
        check(fresh.definitions()[1].name == definitions[1].name &&
            fresh.resolve(definitions[1].name,"{}").description.instance != instance->generation(),"reload lost stable name or changed instance identity");
        rejects([&] { (void)snapshot.prepare(attempt,{"old",definitions[1].name,"{}"}); });
        check(replacement->shutdown() == ShutdownResult::closed,"idle registration pinned native code");
        const auto legacy = legacy_package(argv[3],std::filesystem::path(argv[2])/"legacy");
        auto old = std::make_shared<PluginInstance>(legacy,argv[2],channel);
        check(!old->create() && !old->start(),"legacy instance start failed");
        PluginTools optional(old,catalog);
        check(optional.ids().empty() && old->shutdown() == ShutdownResult::closed,"tool capability became required for legacy plugins");
        std::cout << "Native tool registration checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
