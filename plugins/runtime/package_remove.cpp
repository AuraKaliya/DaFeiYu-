#include <dafeiyu/plugin_package.hpp>
#include "package_files.hpp"
#include <regex>
#include <stdexcept>

namespace dfy::plugins {
namespace fs = std::filesystem;
// 先固定唯一可见包和同 ID 私有残留，再将包退出发现目录；部分清理可由下一次调用继续。
// 步骤：校验 ID 及根 → 收集唯一包和有界残留 → 校验包并私有改名 → 逐项清理或保留诊断。
RemovalReceipt remove_package(const fs::path& root, const std::string& id) {
    if (root.empty() || id.size() > 64 || !std::regex_match(id,std::regex(R"([a-z][a-z0-9]*([._-][a-z0-9]+)*)")) ||
        std::regex_match(id,std::regex(R"((con|prn|aux|nul|(com|lpt)[1-9])(\..*)?)")))
        throw std::invalid_argument("package: valid plugin id required");
    if (!fs::exists(fs::symlink_status(root))) return {};
    const auto base = files::directory(root);
    const auto prefix = ".removed-"+std::to_string(id.size())+"-"+id+"-";
    std::vector<fs::path> pending; std::optional<Manifest> visible;
    for (const auto& entry : fs::directory_iterator(base)) {
        const auto name_bytes = entry.path().filename().u8string(); const std::string name(name_bytes.begin(),name_bytes.end());
        if (name.starts_with(prefix)) {
            if (pending.size() == 64) throw std::length_error("package: too many removal remnants");
            pending.push_back(entry.path()); continue;
        }
        if (name.starts_with('.')) continue;
        std::optional<Manifest> candidate;
        try { candidate = load_manifest(entry.path()); } catch (const std::runtime_error&) { continue; }
        if (candidate->id == id) {
            if (visible) throw std::runtime_error("package: duplicate id is ambiguous; resolve directory conflict first");
            visible = std::move(candidate);
        }
    }
    RemovalReceipt result{visible.has_value() || !pending.empty(),{}};
    pending.reserve(pending.size()+1); result.residual.reserve(pending.size()+1);
    if (visible) {
        (void)files::inspect(visible->directory);
        const auto private_path = files::temporary(base,prefix);
        if (!fs::remove(private_path)) throw std::runtime_error("package: cannot prepare removal destination");
        pending.push_back(private_path);
        files::move(base,visible->directory,private_path);
    }
    for (const auto& path : pending) {
        try { files::erase(base,path); }
        catch (const std::exception&) { result.residual.push_back(path); }
    }
    return result;
}
}
