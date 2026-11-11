#include <dafeiyu/plugin_registry.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using namespace dfy::plugins;

// 在注册表行为不符合契约时报告测试失败。
void check(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}

// 写入隔离测试文件，检查文件是否完整关闭。
void write(const fs::path& path, const std::string& body) {
    std::ofstream file(path, std::ios::binary);
    file << body;
    file.close();
    check(static_cast<bool>(file), "cannot write test file");
}

// 创建或更新只有描述和文本入口的测试包，不产生可执行 DLL。
// 步骤：准备目录和入口文件 → 写入指定 ID 与版本的清单。
void package(const fs::path& path, const std::string& id, const std::string& version = "1") {
    fs::create_directories(path);
    write(path / "entry.dll", "not a DLL");
    write(path / "plugin.json", nlohmann::json{{"manifest_version", 1}, {"id", id}, {"name", "测试插件"},
        {"version", version}, {"abi_version", 1}, {"entry", "entry.dll"}}.dump());
}

// 仅删除测试包中已知的两个文件与空目录，不执行递归删除。
void erase_package(const fs::path& path) {
    fs::remove(path / "entry.dll");
    fs::remove(path / "plugin.json");
    check(fs::remove(path), "test package not removed");
}
}

// 验证显式发现、坏包隔离、冲突拒绝和刷新前后快照的独立性。
// 步骤：检查空目录 → 混合发现并制造冲突 → 增删更新并核对快照 → 模拟根目录故障及恢复。
int main(int argc, char** argv) {
    try {
        check(argc == 2, "expected work directory");
        fs::create_directories(argv[1]);
        fs::path run;
        for (unsigned i = 1; ; ++i) {
            run = fs::path(argv[1]) / std::to_string(i);
            if (fs::create_directory(run)) {
                break;
            }
        }
        const auto root = run / u8"插件 目录";
        Registry registry(root);
        registry.refresh();
        check(registry.snapshot()->packages.empty() && registry.snapshot()->diagnostics.empty() && !fs::exists(root),
            "missing root not empty or created on disk");
        fs::create_directory(root);
        registry.refresh();
        check(registry.snapshot()->changes.empty(), "empty refresh reported changes");

        package(root / "z", "sample.z");
        package(root / "a", "sample.a");
        package(root / ".staging/hidden", "sample.hidden");
        fs::create_directory(root / "bad");
        write(root / "bad/plugin.json", "{invalid-json}");
        write(root / "notes.txt", "ignored loose file");
        fs::create_directory_symlink(fs::absolute(root / "a"), root / "linked");
        registry.refresh();
        const auto original = registry.snapshot();
        check(original->packages.size() == 2 && original->packages.begin()->first == "sample.a" &&
                  original->diagnostics.size() == 2 && original->diagnostics[0].directory.filename() == "bad" &&
                  original->diagnostics[1].directory.filename() == "linked" && original->changes.size() == 2,
            "discovery order, bad package isolation or reserved directory failed");
        registry.refresh();
        check(registry.snapshot()->changes.empty(), "unchanged refresh reported changes");

        package(root / "duplicate1", "sample.a");
        package(root / "duplicate2", "sample.a");
        registry.refresh();
        auto current = registry.snapshot();
        check(current->packages.size() == 1 && current->packages.contains("sample.z") && current->diagnostics.size() == 5 &&
                  current->changes.size() == 1 && current->changes[0].id == "sample.a" &&
                  current->changes[0].kind == ChangeKind::removed, "duplicate ID kept a scan-order winner");
        for (const auto& diagnostic : current->diagnostics) {
            if (diagnostic.directory.filename() != "bad" && diagnostic.directory.filename() != "linked") {
                check(diagnostic.message == "id: duplicate plugin id sample.a", "duplicate package lacks diagnostic");
            }
        }
        erase_package(root / "duplicate1");
        erase_package(root / "duplicate2");
        registry.refresh();
        check(registry.snapshot()->changes.size() == 1 && registry.snapshot()->changes[0].kind == ChangeKind::added,
            "resolved conflict not rediscovered");

        package(root / "a", "sample.a", "2");
        check(registry.snapshot()->packages.at("sample.a").version == "1", "snapshot changed without refresh");
        registry.refresh();
        check(registry.snapshot()->changes.size() == 1 && registry.snapshot()->changes[0].kind == ChangeKind::changed &&
                  registry.snapshot()->packages.at("sample.a").version == "2" &&
                  original->packages.at("sample.a").version == "1", "updated description corrupted retained snapshot");
        erase_package(root / "z");
        package(root / "new", "sample.new");
        registry.refresh();
        current = registry.snapshot();
        check(current->changes.size() == 2 && current->changes[0].id == "sample.new" &&
                  current->changes[0].kind == ChangeKind::added && current->changes[1].id == "sample.z" &&
                  current->changes[1].kind == ChangeKind::removed, "add or remove change missing");

        const auto saved = run / "saved";
        check(fs::canonical(root).parent_path() == fs::canonical(run) &&
                  fs::weakly_canonical(saved).parent_path() == fs::canonical(run), "move outside test directory");
        fs::rename(root, saved);
        write(root, "not a directory");
        bool failed = false;
        try {
            registry.refresh();
        } catch (const std::runtime_error&) {
            failed = true;
        }
        check(failed && registry.snapshot() == current, "failed root scan replaced prior snapshot");
        fs::remove(root);
        check(fs::canonical(saved).parent_path() == fs::canonical(run) &&
                  fs::weakly_canonical(root).parent_path() == fs::canonical(run), "restore outside test directory");
        fs::rename(saved, root);
        registry.refresh();
        check(registry.snapshot()->changes.empty(), "recovered root changed valid packages");
        std::shared_ptr<const Catalog> retained;
        {
            Registry temporary(root);
            temporary.refresh();
            retained = temporary.snapshot();
        }
        check(retained->packages.at("sample.a").version == "2", "snapshot tied to registry lifetime");
        std::cout << "Plugin registry checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
