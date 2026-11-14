#include <dafeiyu/plugin_package.hpp>
#include "package_files.hpp"
#include <algorithm>
#include <regex>
#include <stdexcept>

namespace dfy::plugins {
namespace fs = std::filesystem;
// 有界清理同 ID 旧残留后再移除唯一可见包，任何残留数量都可通过后续调用继续推进。
// 步骤：校验 ID 及根 → 固定可见包及首批残留 → 清理旧批并报告未完成 → 按需私有改名及清理当前包。
RemovalReceipt remove_package(const fs::path& root, const std::string& id) {
    if (root.empty() || id.size() > 64 || !std::regex_match(id,std::regex(R"([a-z][a-z0-9]*([._-][a-z0-9]+)*)")) ||
        std::regex_match(id,std::regex(R"((con|prn|aux|nul|(com|lpt)[1-9])(\..*)?)")))
        throw std::invalid_argument("package: valid plugin id required");
    if (!fs::exists(fs::symlink_status(root))) return {};
    const auto base = files::directory(root);
    const auto prefix = ".removed-"+std::to_string(id.size())+"-"+id+"-";
    std::vector<fs::path> pending; std::optional<Manifest> visible; bool more_pending = false;
    for (const auto& entry : fs::directory_iterator(base)) {
        const auto name_bytes = entry.path().filename().u8string(); const std::string name(name_bytes.begin(),name_bytes.end());
        if (name.starts_with(prefix)) {
            if (pending.size() < 64) pending.push_back(entry.path());
            else more_pending = true;
            continue;
        }
        if (name.starts_with('.')) continue;
        std::optional<Manifest> candidate;
        try { candidate = load_manifest(entry.path()); } catch (const std::runtime_error&) { continue; }
        if (candidate->id == id) {
            if (visible) throw std::runtime_error("package: duplicate id is ambiguous; resolve directory conflict first");
            visible = std::move(candidate);
        }
    }
    RemovalReceipt result{visible.has_value() || !pending.empty(),{},visible.has_value(),more_pending};
    result.residual.reserve(std::max<std::size_t>(pending.size(),1));
    if (visible) (void)files::inspect(visible->directory);
    for (const auto& path : pending) {
        try { files::erase(base,path); }
        catch (const std::exception&) { result.residual.push_back(path); }
    }
    // 先归还旧清理责任，避免每次重装又产生一个残留，最终超过自身恢复能力。
    if (result.more_pending || !result.residual.empty()) return result;

    if (visible) {
        const auto private_path = files::temporary(base,prefix);
        if (!fs::remove(private_path)) throw std::runtime_error("package: cannot prepare removal destination");
        files::move(base,visible->directory,private_path);
        result.package_present = false;
        try { files::erase(base,private_path); }
        catch (const std::exception&) { result.residual.push_back(private_path); }
    }
    return result;
}
}
