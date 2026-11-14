#include <dafeiyu/plugin_package.hpp>
#include <dafeiyu/plugin_config.hpp>
#include "package_files.hpp"
#include <type_traits>

static_assert(std::is_nothrow_move_constructible_v<dfy::plugins::Manifest>);

namespace dfy::plugins {
namespace fs = std::filesystem;
namespace {
// 按实际目录身份检查包含关系，避免大小写或路径拼写绕过源与目标重叠检查。
bool contains(const fs::path& root, fs::path candidate) {
    while (!candidate.empty()) {
        std::error_code error;
        if (fs::equivalent(root,candidate,error)) return true;
        if (error && error != std::errc::no_such_file_or_directory) throw fs::filesystem_error("compare package roots",root,candidate,error);
        const auto parent = candidate.parent_path(); if (parent == candidate) break; candidate = parent;
    }
    return false;
}
// 准备同一清单在另一个目录下的路径副本，全部分配在发布之前完成。
Manifest relocated(const Manifest& source, const fs::path& destination) {
    auto result = source; result.directory = destination;
    result.entry = destination/source.entry.lexically_relative(source.directory);
    if (source.default_config) result.default_config = destination/source.default_config->lexically_relative(source.directory);
    return result;
}
// 检查全部可解析包的实际 ID，目录别名或已有重复 ID 也不能绕过重名拒绝。
// 步骤：检查固定目标 → 枚举可发现子项 → 只对可解析包核对完整 ID。
void reject_existing(const fs::path& root, const std::string& id) {
    if (fs::exists(fs::symlink_status(root/id))) throw std::runtime_error("package: destination already exists");
    for (const auto& entry : fs::directory_iterator(root)) {
        if (entry.path().filename().native().starts_with(L'.')) continue;
        std::optional<Manifest> existing;
        try { existing = load_manifest(entry.path()); } catch (const std::runtime_error&) { continue; }
        if (existing->id == id) throw std::runtime_error("package: plugin id already installed");
    }
}
}
// 使用私有暂存隔离复制过程，只有清单、默认配置及目录树都复核成功后才发布。
// 步骤：检查源树及管理根 → 拒绝重叠和同名 → 暂存复制并复核 → 准备发布结果 → 原子发布或清理暂存。
Manifest install_package(const fs::path& root, const fs::path& source, PackageLimits limits) {
    const auto origin = files::directory(source); const auto manifest = load_manifest(origin);
    const auto items = files::inspect(origin,limits);
    if (root.empty()) throw std::invalid_argument("package: managed root required");
    const auto planned = fs::weakly_canonical(fs::absolute(root).lexically_normal());
    if (contains(origin,planned) || contains(planned,origin)) throw std::invalid_argument("package: source and managed root must not overlap");
    const auto base = files::directory(root,true);
    reject_existing(base,manifest.id);
    const auto stage = files::temporary(base,".install-");
    try {
        for (const auto& item : items) {
            const auto from = origin/item.relative, to = stage/item.relative;
            (void)files::directory(from.parent_path());
            if (item.directory) { (void)files::directory(from); fs::create_directory(to); }
            else {
                if (!fs::is_regular_file(fs::symlink_status(from))) throw std::runtime_error("package: source file changed during copy");
                fs::copy_file(from,to);
            }
        }
        (void)files::inspect(stage,limits); const auto copied = load_manifest(stage);
        if (copied != relocated(manifest,stage)) throw std::runtime_error("package: source definition changed during copy");
        const auto validation = base/(stage.filename().native()+L"-settings-check");
        if (fs::exists(fs::symlink_status(validation))) throw std::runtime_error("package: validation path already exists");
        (void)load_config(copied,validation);
        reject_existing(base,manifest.id);
        auto installed = relocated(copied,base/manifest.id);
        files::move(base,stage,installed.directory); return installed;
    } catch (...) {
        const auto failure = std::current_exception();
        try { files::erase(base,stage); }
        catch (...) {
            const auto bytes = stage.u8string();
            throw std::runtime_error("package: installation failed; private staging remains at "+std::string(bytes.begin(),bytes.end()));
        }
        std::rethrow_exception(failure);
    }
}
}
