#include <dafeiyu/plugin_registry.hpp>
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace dfy::plugins {
namespace fs = std::filesystem;

// 固定目录解析基准，使后续刷新不受进程工作目录变化影响。
// 步骤：拒绝空路径 → 保存规范化绝对路径 → 建立空目录快照。
Registry::Registry(const fs::path& root) {
    if (root.empty()) {
        throw std::invalid_argument("plugins: root path required");
    }
    root_ = fs::absolute(root).lexically_normal();
    if (root_.has_relative_path() && root_.filename().empty()) {
        root_ = root_.parent_path();
    }
    catalog_ = std::make_shared<const Catalog>();
}

// 完整扫描直接子项后替换内存目录，单包错误不影响其他包。
// 步骤：检查根目录并枚举候选 → 隔离坏包并排除全部 ID 冲突 → 比较可用目录差异 → 提交新快照。
void Registry::refresh() {
    auto next = std::make_shared<Catalog>();
    std::vector<fs::path> paths;
    std::vector<Manifest> valid;
    std::map<std::string, std::size_t> counts;
    try {
        std::error_code error;
        const auto status = fs::symlink_status(root_, error);
        if (error && error != std::errc::no_such_file_or_directory) {
            throw std::runtime_error("plugins: cannot inspect root");
        }
        if (fs::exists(status)) {
            if (!fs::is_directory(status)) {
                throw std::runtime_error("plugins: expected root directory without links");
            }
            const auto root = fs::canonical(root_);
            for (const auto& entry : fs::directory_iterator(root)) {
                // 点开头的子项保留给宿主暂存用途，整个子树不参与发现。
                if (!entry.path().filename().native().starts_with('.')) {
                    paths.push_back(entry.path());
                }
            }
            std::sort(paths.begin(), paths.end());

            for (const auto& path : paths) {
                try {
                    if (fs::is_regular_file(fs::symlink_status(path))) {
                        continue;
                    }
                    auto manifest = load_manifest(path);
                    if (manifest.directory.parent_path() != root) {
                        throw std::runtime_error("package: outside plugin root");
                    }
                    ++counts[manifest.id];
                    valid.push_back(std::move(manifest));
                } catch (const fs::filesystem_error&) {
                    next->diagnostics.push_back({path, "package: cannot inspect path"});
                } catch (const std::runtime_error& failure) {
                    next->diagnostics.push_back({path, failure.what()});
                }
            }
        }
    } catch (const fs::filesystem_error&) {
        throw std::runtime_error("plugins: cannot scan root");
    }

    for (auto& manifest : valid) {
        if (counts.at(manifest.id) != 1) {
            next->diagnostics.push_back({manifest.directory, "id: duplicate plugin id " + manifest.id});
        } else {
            const auto id = manifest.id;
            next->packages.emplace(id, std::move(manifest));
        }
    }
    std::sort(next->diagnostics.begin(), next->diagnostics.end(),
        [](const auto& left, const auto& right) { return left.directory < right.directory; });

    for (const auto& [id, previous] : catalog_->packages) {
        const auto current = next->packages.find(id);
        if (current == next->packages.end()) {
            next->changes.push_back({ChangeKind::removed, id});
        } else if (previous != current->second) {
            next->changes.push_back({ChangeKind::changed, id});
        }
    }
    for (const auto& [id, manifest] : next->packages) {
        if (!catalog_->packages.contains(id)) {
            next->changes.push_back({ChangeKind::added, id});
        }
    }
    std::sort(next->changes.begin(), next->changes.end(),
        [](const auto& left, const auto& right) { return left.id < right.id; });
    catalog_ = std::move(next);
}
}
