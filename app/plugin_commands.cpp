#include "plugin_commands.hpp"
#include <algorithm>
#include <iostream>

namespace dfy::app {
// 双引号只负责组合参数，反斜杠始终是普通路径字符，不执行 shell 转义或命令替换。
// 步骤：检查总长度 → 按引号状态累计参数 → 核对闭合及数量 → 返回完整词表。
std::vector<std::string> command_words(std::string_view command) {
    if (command.size() > 64*1024) throw std::length_error("command: input exceeds 64 KiB");
    std::vector<std::string> words; std::string word; bool quoted = false, started = false;
    for (const char value : command) {
        if (value == '"') { quoted = !quoted; started = true; }
        else if (!quoted && (value == ' ' || value == '\t')) {
            if (started) { words.push_back(std::move(word)); word.clear(); started = false; }
        } else { word.push_back(value); started = true; }
        if (words.size() > 8) throw std::length_error("command: too many arguments");
    }
    if (quoted) throw std::invalid_argument("command: unmatched double quote");
    if (started) words.push_back(std::move(word));
    if (words.size() > 8) throw std::length_error("command: too many arguments");
    return words;
}
namespace {
// 把生命周期转换为用户可识别状态，不暴露内部对象或配置。
const char* phase(plugins::InstancePhase value) {
    switch (value) {
    case plugins::InstancePhase::active: return "running";
    case plugins::InstancePhase::loaded:
    case plugins::InstancePhase::created: return "loaded";
    case plugins::InstancePhase::stopping:
    case plugins::InstancePhase::destroyed: return "stopping";
    case plugins::InstancePhase::unloaded: return "unloaded";
    case plugins::InstancePhase::broken: return "needs attention";
    }
    return "unknown";
}
// 列出磁盘包与仍存活实例，目录变化不会隐藏仍需回收的已加载实例。
// 步骤：取得一致快照 → 列出包及运行状态 → 补充缺失包实例 → 报告独立目录诊断。
void list(plugins::PluginManager& manager, std::ostream& output) {
    const auto catalog = manager.catalog(); const auto loaded = manager.loaded();
    if (!manager.directory_error().empty()) output << "Plugin directory unavailable: " << manager.directory_error() << '\n';
    if (catalog->packages.empty() && loaded.empty()) output << "No plugins installed or loaded.\n";
    for (const auto& [id,package] : catalog->packages) {
        const auto instance = std::find_if(loaded.begin(),loaded.end(),[&](const auto& item) { return item.id == id; });
        output << id << " [" << (instance == loaded.end() ? "available" : instance->error ? "needs cleanup" : phase(instance->phase)) << "] " << package.version << " - " << package.name << '\n';
    }
    for (const auto& item : loaded) if (!catalog->packages.contains(item.id)) output << item.id << " [" << phase(item.phase) << "] package missing or changed\n";
    for (const auto& diagnostic : catalog->diagnostics) {
        const auto name = diagnostic.directory.filename().u8string();
        output << "Unavailable package " << std::string(name.begin(),name.end()) << ": " << diagnostic.message << '\n';
    }
}
}
// 插件命令与问答分流，生命周期拒绝保留实例并给出可重试的诊断。
// 步骤：匹配命令族并解析 → 按精确参数分派 → 映射卸载结果或错误 → 返回本地处理标记。
bool plugin_command(plugins::PluginManager& manager, std::string_view command, std::ostream& output, std::ostream& errors) {
    if (!command.starts_with("/plugins") || (command.size() > 8 && command[8] != ' ' && command[8] != '\t')) return false;
    try {
        const auto words = command_words(command);
        if (words.size() == 2 && words[1] == "list") list(manager,output);
        else if (words.size() == 2 && words[1] == "refresh") { manager.refresh(); output << "Plugin directory refreshed.\n"; list(manager,output); }
        else if (words.size() == 3 && words[1] == "install") {
            const auto installed = manager.install(std::filesystem::path(std::u8string(words[2].begin(),words[2].end())));
            output << "Installed " << installed.id << ". Use /plugins load " << installed.id << " to start it.\n";
            if (!manager.directory_error().empty()) errors << "Package installed; refresh the directory again: " << manager.directory_error() << '\n';
        }
        else if (words.size() == 3 && words[1] == "load") {
            const auto status = manager.load(words[2]);
            if (!status) output << "Loaded " << words[2] << ".\n";
            else if (status == DFY_V2_BUSY) errors << "Plugin is already loaded or finishing cleanup.\n";
            else if (status == DFY_V2_RESOURCE_EXHAUSTED) errors << "Plugin capacity is full.\n";
            else {
                const auto loaded = manager.loaded();
                const bool kept = std::any_of(loaded.begin(),loaded.end(),[&](const auto& item) { return item.id == words[2]; });
                errors << (kept ? "Plugin could not start; unload it to retry cleanup.\n" : "Plugin could not start; cleanup completed.\n");
            }
        } else if ((words.size() == 3 || (words.size() == 4 && words[3] == "--linked")) && words[1] == "unload") {
            const auto result = manager.unload(words[2],words.size() == 4 ? plugins::DependencyStop::linked : plugins::DependencyStop::refuse_dependents);
            switch (result) {
            case plugins::ShutdownResult::closed: output << "Unloaded " << words[2] << ".\n"; break;
            case plugins::ShutdownResult::pending: output << "Plugin is stopping; the host will keep checking.\n"; break;
            case plugins::ShutdownResult::busy: errors << "Plugin is busy; try unloading after the current call returns.\n"; break;
            case plugins::ShutdownResult::in_use: errors << "Plugin is still in use. Stop its users or choose --linked.\n"; break;
            case plugins::ShutdownResult::failed: errors << "Plugin stop failed; its state is kept for another unload attempt.\n"; break;
            }
        } else errors << "Usage: /plugins list | refresh | install <directory> | load <id> | unload <id> [--linked]\n";
    } catch (const std::exception& error) { errors << error.what() << '\n'; }
    catch (...) { errors << "Plugin command failed.\n"; }
    return true;
}
}
