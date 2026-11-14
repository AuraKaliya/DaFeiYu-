#include "plugin_commands.hpp"
#include <iostream>
#include <sstream>
namespace {
namespace fs = std::filesystem;
using namespace dfy::plugins;
using namespace dfy::app;
// 在命令解析、本地分流或实例状态违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 创建本轮独立包目录，不覆盖已有测试或用户数据。
fs::path workspace(const fs::path& root) {
    fs::create_directories(root);
    for (unsigned i = 1;; ++i) { const auto path = root/std::to_string(i); if (fs::create_directory(path)) return path; }
}
}
// 验证命令词法及真实包的列表、刷新、加载、拒绝和卸载。
// 步骤：核对中文带空格路径 → 拒绝非法参数 → 管理真实观察包 → 检查未知命令分流。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 3,"observer package and work path required");
        const auto words = command_words(R"(/plugins install "D:\中文 目录\观察包")");
        check(words.size() == 3 && words[2] == R"(D:\中文 目录\观察包)","command parser changed Windows path");
        bool quote = false, many = false;
        try { (void)command_words("/plugins load \"bad"); } catch (const std::invalid_argument&) { quote = true; }
        try { (void)command_words("a b c d e f g h i"); } catch (const std::length_error&) { many = true; }
        check(quote && many,"command parser accepted malformed input");
        const auto run = workspace(argv[2]); const auto root = run/"plugins"; fs::create_directory(root);
        auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),2,2);
        PluginManager manager(root,run/"data",channel,2); std::ostringstream output, errors;
        check(plugin_command(manager,"/plugins list",output,errors) && output.str().find("No plugins") != std::string::npos,"empty command catalog wrong");
        const auto source = run/u8"来源 包"; fs::copy(fs::canonical(argv[1]),source,fs::copy_options::recursive);
        const auto source_bytes = source.u8string();
        plugin_command(manager,"/plugins install \""+std::string(source_bytes.begin(),source_bytes.end())+"\"",output,errors);
        check(manager.catalog()->packages.size() == 1 && manager.loaded().empty() && errors.str().empty(),"quoted install command failed or loaded automatically");
        plugin_command(manager,"/plugins refresh",output,errors); check(manager.loaded().empty(),"refresh auto-loaded plugin");
        plugin_command(manager,"/plugins load example.observer",output,errors);
        check(manager.loaded().size() == 1 && manager.loaded()[0].phase == InstancePhase::active && errors.str().empty(),"load command failed");
        plugin_command(manager,"/plugins list",output,errors); check(output.str().find("[running]") != std::string::npos,"list lost runtime status");
        auto pin = manager.instance("example.observer")->retain_code(); plugin_command(manager,"/plugins unload example.observer",output,errors);
        check(manager.loaded().size() == 1 && errors.str().find("still in use") != std::string::npos,"unload command forced a dependency");
        const auto package_path = manager.catalog()->packages.at("example.observer").directory;
        plugin_command(manager,"/plugins remove example.observer",output,errors);
        check(fs::exists(package_path) && manager.loaded().size() == 1 && errors.str().find("Package kept") != std::string::npos,"remove command changed busy package");
        pin.reset(); plugin_command(manager,"/plugins unload example.observer",output,errors); check(manager.loaded().empty(),"unload command leaked instance");
        plugin_command(manager,"/plugins remove example.observer",output,errors);
        check(!fs::exists(package_path) && manager.catalog()->packages.empty(),"remove command did not update disk and catalog");
        const auto before = manager.catalog(); plugin_command(manager,"/plugins load missing",output,errors);
        check(manager.catalog() == before && !plugin_command(manager,"/plugins-extra list",output,errors),"command error changed catalog or consumed other command family");
        check(manager.close(),"command manager failed to close"); std::cout << "Plugin command checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
