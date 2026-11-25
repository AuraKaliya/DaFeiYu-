#pragma once
#include <dafeiyu/plugin_manifest.hpp>
#include <map>
#include <memory>
#include <vector>

namespace dfy::plugins {
struct Diagnostic {
    std::filesystem::path directory;
    std::string message;
};
enum class ChangeKind { added, removed, changed };
struct Change {
    ChangeKind kind;
    std::string id;
};
struct Catalog {
    std::map<std::string, Manifest> packages;
    std::vector<Diagnostic> diagnostics;
    std::vector<Change> changes;
};

// 单线程目录注册表，只保存包描述；不持有 DLL 或已加载实例。
class Registry {
  public:
    // 固定非空根目录的绝对路径并创建空快照，不立即扫描或创建目录。
    explicit Registry(const std::filesystem::path& root);
    // 显式扫描并替换快照；坏包独立诊断，根目录扫描失败抛错且保留旧快照。
    void refresh();
    // 返回不可变目录快照，调用方持有期间不受后续刷新或注册表销毁影响。
    std::shared_ptr<const Catalog> snapshot() const noexcept { return catalog_; }

  private:
    std::filesystem::path root_;
    std::shared_ptr<const Catalog> catalog_;
};
}
