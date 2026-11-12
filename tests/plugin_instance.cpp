#include <dafeiyu/plugin_instance.hpp>
#include <windows.h>
#include <fstream>
#include <iostream>

namespace {
namespace fs = std::filesystem;
using namespace dfy::plugins;
// 在实例阶段、失败保留或回收条件不满足时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 建立独立真实目录包，避免复用先前运行的工作文件。
// 步骤：创建新目录 → 复制 DLL 并写清单 → 读取正式描述。
Manifest package(const fs::path& dll, const fs::path& root) {
    fs::create_directories(root);
    fs::path run;
    for (unsigned i = 1;; ++i) { run = root / std::to_string(i); if (fs::create_directory(run)) break; }
    fs::copy_file(dll, run / "entry.dll");
    std::ofstream file(run / "plugin.json");
    file << R"({"manifest_version":1,"id":"test.instance","name":"instance","version":"1","abi_version":2,"entry":"entry.dll"})";
    file.close(); check(static_cast<bool>(file), "manifest write failed");
    return load_manifest(run);
}
}
// 通过真实 C 插件验证有界实例、关停阶段及离开作用域后的失败保留。
// 步骤：验证活动和外部依赖拒绝 → 验证 Pending 与单次销毁 → 验证部分启动失败 → 保留并重试失败实例。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 3, "expected lifecycle DLL and work directory");
        const auto manifest = package(fs::canonical(argv[1]), argv[2]);
        dfy::platform::DynamicLibrary control(manifest.entry);
        const auto mode = reinterpret_cast<void (DFY_V2_CALL*)(uint32_t)>(control.symbol("dfy_lifecycle_mode"));
        const auto count = reinterpret_cast<uint32_t (DFY_V2_CALL*)(uint32_t)>(control.symbol("dfy_lifecycle_count"));
        auto store = std::make_shared<dfy::events::EventStore>(8, 0);
        auto channel = std::make_shared<dfy::events::BroadcastChannel>(store, 4, 2);
        const auto data = manifest.directory / "userdata";
        {
            PluginInstance instance(manifest, data, channel);
            check(instance.phase() == InstancePhase::loaded && instance.create() == DFY_V2_OK && instance.start() == DFY_V2_OK,
                "instance did not start");
            auto external = instance.retain_code();
            {
                auto active = external.enter();
                check(instance.shutdown() == ShutdownResult::busy && instance.phase() == InstancePhase::active, "busy shutdown mutated state");
            }
            check(instance.shutdown() == ShutdownResult::in_use && instance.phase() == InstancePhase::active, "dependency shutdown mutated state");
            external.reset(); mode(4);
            check(instance.shutdown() == ShutdownResult::pending && instance.phase() == InstancePhase::stopping && !count(3),
                "accepted stop destroyed instance");
            check(instance.shutdown() == ShutdownResult::closed && instance.shutdown() == ShutdownResult::closed && count(3) == 1 &&
                !channel->size() && !store->references(), "shutdown replayed or leaked resources");
        }
        {
            PluginInstance instance(manifest, data, channel);
            check(instance.create() == DFY_V2_OK, "second create failed");
            mode(2);
            check(instance.start() == DFY_V2_FAILED && !channel->size(), "partial start was not quiesced");
            check(instance.shutdown() == ShutdownResult::closed && count(3) == 2, "partial start cleanup failed");
        }
        {
            auto instance = std::make_unique<PluginInstance>(manifest, data, channel);
            check(instance->create() == DFY_V2_OK && instance->start() == DFY_V2_OK, "retention setup failed");
            mode(5);
            check(instance->shutdown() == ShutdownResult::failed && count(3) == 2, "stop failure destroyed instance");
            instance.reset();
            const auto retained = PluginInstance::retained();
            check(retained.size() == 1 && retained.front().id == "test.instance" && retained.front().error == DFY_V2_FAILED &&
                PluginInstance::retry_retained() == 1 && count(3) == 2, "failed state not retained");
            mode(0);
            check(PluginInstance::retry_retained() == 0 && PluginInstance::retained().empty() && count(3) == 3,
                "retained cleanup did not resume once");
        }
        {
            PluginInstance instance(manifest, data, channel);
            mode(1);
            check(instance.create() == DFY_V2_FAILED && instance.phase() == InstancePhase::unloaded && count(3) == 3,
                "failed creation was destroyed or retained unnecessarily");
        }
        control.close();
        check(!GetModuleHandleW(manifest.entry.c_str()), "instance module reference leaked");
        std::cout << "Managed plugin instance checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
