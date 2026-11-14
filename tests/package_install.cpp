#include <dafeiyu/plugin_package.hpp>
#include <dafeiyu/plugin_manager.hpp>
#include <windows.h>
#include <fstream>
#include <iostream>
namespace {
namespace fs = std::filesystem;
using namespace dfy::plugins;
// 在安装发布、回滚或用户配置隔离违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 验证无效包操作被拒绝，失败后由调用方核对无部分发布。
template<class Action>
void rejects(Action action) { bool rejected = false; try { action(); } catch (const std::exception&) { rejected = true; } check(rejected,"invalid install accepted"); }
// 为本次安装验收创建唯一目录，避免覆盖既有文件。
fs::path workspace(const fs::path& root) {
    fs::create_directories(root);
    for (unsigned i = 1;; ++i) { auto path = root/std::to_string(i); if (fs::create_directory(path)) return path; }
}
// 写入隔离测试文件并核对关闭结果。
void write(const fs::path& path, const char* text) { std::ofstream file(path); file << text; file.close(); check(static_cast<bool>(file),"install fixture write failed"); }
// 验证调用方提高目录深度限制后，同一策略仍可移除和回滚，不意外退回默认上限。
// 步骤：建立 33 层合法包 → 用明确限制安装及移除 → 制造配置失败 → 检查暂存完整回滚。
void custom_limits(const fs::path& original, const fs::path& run) {
    const auto source = run/"deep"; fs::copy(original,source,fs::copy_options::recursive);
    auto current = source;
    for (unsigned i = 0; i < 33; ++i) { current /= "d"; fs::create_directory(current); }
    const PackageLimits limits{64,512*1024*1024,40};
    const auto root = run/"deep-install"; const auto installed = install_package(root,source,limits);
    const auto removed = remove_package(root,installed.id,limits);
    check(removed.residual.empty() && !removed.package_present && fs::is_empty(root),"custom install limits were lost during removal");
    write(source/"defaults.json","{"); bool failed = false;
    try { (void)install_package(root,source,limits); } catch (const std::exception&) { failed = true; }
    check(failed && fs::is_empty(root),"custom install limits were lost during staging rollback");
}
}
// 验证真实本地包的安全暂存发布、不自动加载、失败清理及同 ID 拒绝。
// 步骤：安装到中文目录 → 保持用户设置并检查未加载 → 拒绝重复和超量 → 检查配置失败回滚及链接拒绝。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 3,"observer package and workspace required"); const auto run = workspace(argv[2]);
        const auto source = run/u8"来源 包"; fs::copy(fs::canonical(argv[1]),source,fs::copy_options::recursive);
        const auto root = run/u8"安装 根", data = run/"data"; fs::create_directories(data/"plugins"/"example.observer");
        write(data/"plugins"/"example.observer"/"settings.json","keep user settings");
        auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),2,2);
        PluginManager manager(root,data,channel,2); const auto installed = manager.install(source);
        check(installed.id == "example.observer" && installed.directory == fs::canonical(root)/installed.id &&
            manager.catalog()->packages.size() == 1 && manager.loaded().empty() && !GetModuleHandleW(installed.entry.c_str()),"install loaded code or published wrong destination");
        std::ifstream settings(data/"plugins"/"example.observer"/"settings.json"); std::string saved; std::getline(settings,saved);
        check(saved == "keep user settings","installation changed user configuration");
        rejects([&] { (void)manager.install(source); });
        rejects([&] { (void)install_package(run/"small",source,{2,512*1024*1024,32}); });
        check(!fs::exists(run/"small"),"preflight capacity rejection created partial package root");
        const auto existing = run/"aliases"; fs::create_directory(existing); fs::copy(source,existing/"different-directory",fs::copy_options::recursive);
        rejects([&] { (void)install_package(existing,source); });
        check(!fs::exists(existing/"example.observer"),"directory alias bypassed duplicate id refusal");
        rejects([&] { (void)install_package(source/"nested-root",source); });
        check(!fs::exists(source/"nested-root"),"overlap refusal modified source package");

        const auto malformed = run/"malformed"; fs::copy(fs::canonical(argv[1]),malformed,fs::copy_options::recursive); write(malformed/"defaults.json","{");
        rejects([&] { (void)install_package(run/"failed",malformed); });
        check(fs::is_empty(run/"failed"),"failed default validation left staging or a discoverable half-package");
        const auto linked = run/"linked"; fs::copy(fs::canonical(argv[1]),linked,fs::copy_options::recursive);
        write(run/"outside.txt","outside"); fs::create_symlink(run/"outside.txt",linked/"extra.txt");
        rejects([&] { (void)install_package(run/"link-target",linked); });
        check(fs::exists(run/"outside.txt") && !fs::exists(run/"link-target"),"link rejection followed external data or published partly");
        check(manager.close(),"install manager could not close"); custom_limits(fs::canonical(argv[1]),run);
        std::cout << "Package install checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
