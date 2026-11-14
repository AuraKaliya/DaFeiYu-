#include <dafeiyu/plugin_manager.hpp>
#include <windows.h>
#include <fstream>
#include <iostream>
namespace {
namespace fs = std::filesystem;
using namespace dfy::plugins;
// 在忙碌移除、用户配置保持或残留重试违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 创建独立验收目录，不复用或删除用户目录。
fs::path workspace(const fs::path& root) {
    fs::create_directories(root);
    for (unsigned i = 1;; ++i) { const auto path = root/std::to_string(i); if (fs::create_directory(path)) return path; }
}
// 验证超过单批上限的旧移除残留仍能逐批清理，未退出发现目录的当前包不能误报已移除。
// 步骤：准备旧版可能留下的 65 个残留 → 清理首批并保持当前包 → 重试完成剩余目录和当前包。
void many_remnants(const fs::path& source, const fs::path& run) {
    const auto root = run/"many-remnants";
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,1);
    PluginManager manager(root,run/"many-user",channel,1); const auto installed = manager.install(source);
    for (unsigned i = 0; i < 65; ++i) {
        const auto directory = root/(".removed-16-example.observer-review-"+std::to_string(i));
        fs::create_directory(directory); std::ofstream file(directory/"old.txt"); file << "old";
    }
    const auto first = manager.remove(installed.id);
    check(first.files.package_present && first.files.more_pending && first.files.residual.empty() && fs::exists(installed.directory),
        "bounded cleanup hid an unprocessed batch or removed the visible package early");
    const auto second = manager.remove(installed.id);
    check(!second.files.package_present && !second.files.more_pending && second.files.residual.empty() && fs::is_empty(root),
        "remnant limit permanently blocked cleanup retry");
    check(manager.close(),"remnant batch manager failed to close");
}
}
// 验证卸载后移除、忙碌原样拒绝、清理残留重试及坏二进制包移除。
// 步骤：安装并加载真实包 → 拒绝使用中移除 → 完成卸载和移除 → 制造只读残留并重试 → 移除加载失败包。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 3,"observer package and workspace required"); const auto run = workspace(argv[2]); const auto root = run/"plugins", data = run/"data";
        fs::create_directories(data/"plugins"/"example.observer"); const auto settings = data/"plugins"/"example.observer"/"settings.json";
        { std::ofstream file(settings); file << "{}"; }
        auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),2,2);
        PluginManager manager(root,data,channel,2); const auto installed = manager.install(argv[1]); check(!manager.load(installed.id),"removal setup load failed");
        auto pin = manager.instance(installed.id)->retain_code(); const auto blocked = manager.remove(installed.id);
        check(blocked.status == ShutdownResult::in_use && fs::exists(installed.entry) && manager.instance(installed.id)->phase() == InstancePhase::active,
            "busy removal changed package or instance");
        pin.reset(); const auto removed = manager.remove(installed.id);
        check(removed.status == ShutdownResult::closed && removed.files.residual.empty() && !fs::exists(installed.directory) &&
            fs::exists(settings) && manager.loaded().empty() && manager.catalog()->packages.empty(),"removal leaked package or erased user configuration");

        const auto second = manager.install(argv[1]); check(second.default_config.has_value(),"readonly cleanup fixture lacks config");
        const auto flags = GetFileAttributesW(second.default_config->c_str());
        check(SetFileAttributesW(second.default_config->c_str(),flags|FILE_ATTRIBUTE_READONLY),"cannot create readonly cleanup fixture");
        const auto partial = manager.remove(second.id);
        check(partial.files.residual.size() == 1 && manager.catalog()->packages.empty() && !fs::exists(second.directory),"partial cleanup still discoverable or reported complete");
        const auto remaining = partial.files.residual.front()/"defaults.json";
        bool different = false;
        try { (void)manager.remove("example"); } catch (const std::invalid_argument&) { different = true; }
        check(different && fs::exists(remaining),"shorter plugin id consumed another package's removal residue");
        check(fs::exists(remaining) && SetFileAttributesW(remaining.c_str(),flags),"residual file disappeared or could not be repaired");
        const auto retried = manager.remove(second.id);
        check(retried.files.found && retried.files.residual.empty() && !fs::exists(partial.files.residual.front()),"same-id cleanup retry lost partial package");

        const auto corrupt = run/"bad binary"; fs::copy(fs::canonical(argv[1]),corrupt,fs::copy_options::recursive);
        const auto bad = load_manifest(corrupt); { std::ofstream file(bad.entry,std::ios::binary|std::ios::trunc); file << "not a DLL"; }
        manager.install(corrupt); bool failed = false;
        try { failed = manager.load(bad.id) != DFY_V2_OK; } catch (const std::exception&) { failed = true; }
        check(failed && manager.remove(bad.id).files.residual.empty() && fs::exists(settings),"failed-load package could not be removed independently");
        check(manager.close(),"removal manager did not close"); many_remnants(argv[1],run);
        std::cout << "Package removal checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
