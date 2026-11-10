#include <dafeiyu/plugin_manifest.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;

// 在清单校验行为不符合契约时报告测试失败。
void check(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}

// 写入隔离测试文件并确认内容完整落入输出流。
void write(const fs::path& path, const std::string& body) {
    std::ofstream file(path, std::ios::binary);
    file << body;
    file.close();
    check(static_cast<bool>(file), "cannot write test file");
}

// 验证无效清单被拒绝，且错误不会回显文件内容。
// 步骤：写入待测清单 → 捕获解析失败 → 核对错误类别和信息边界。
void rejects(const fs::path& root, const std::string& body, const std::string& expected) {
    write(root / "plugin.json", body);
    try {
        (void)dfy::plugins::load_manifest(root);
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        check(message.find(expected) != std::string::npos, "unexpected manifest error");
        check(message.find("private-test-marker") == std::string::npos, "manifest contents leaked");
        return;
    }
    throw std::runtime_error("invalid manifest accepted");
}
}

// 验证清单字段、包内文件约束和实际链接拒绝，不加载测试入口。
// 步骤：创建独立中文目录包 → 验证合法描述 → 覆盖非法字段和路径 → 检查文件及目录链接。
int main(int argc, char** argv) {
    try {
        check(argc == 2, "expected work directory");
        const fs::path work(argv[1]);
        fs::create_directories(work);
        fs::path run;
        for (unsigned i = 1; ; ++i) {
            run = work / std::to_string(i);
            if (fs::create_directory(run)) {
                break;
            }
        }
        const auto root = run / u8"中文 插件";
        fs::create_directories(root / u8"内容");
        fs::create_directory(root / "directory.dll");
        write(root / u8"内容/入口.dll", "not a DLL");
        write(root / "defaults.json", "not parsed in this stage");
        const Json valid{{"manifest_version", 1}, {"id", "example.observer"}, {"name", "事件观察示例"},
            {"version", "0.1.0-preview"}, {"abi_version", 1}, {"entry", "内容/入口.dll"}};
        write(root / "plugin.json", "\xEF\xBB\xBF" + valid.dump());
        auto loaded = dfy::plugins::load_manifest(root);
        check(loaded.directory == fs::canonical(root) && loaded.id == "example.observer" &&
                  loaded.name == "事件观察示例" && loaded.version == "0.1.0-preview" && loaded.abi_version == 1 &&
                  loaded.entry == fs::canonical(root / u8"内容/入口.dll") && !loaded.default_config,
            "valid manifest changed");
        auto value = valid;
        value["default_config"] = "defaults.json";
        value["abi_version"] = 4294967295ULL;
        write(root / "plugin.json", value.dump());
        loaded = dfy::plugins::load_manifest(root);
        check(loaded.default_config == fs::canonical(root / "defaults.json") && loaded.abi_version == 4294967295U,
            "optional config or future ABI rejected");

        for (const auto* field : {"id", "name", "version"}) {
            const std::size_t limit = std::string(field) == "name" ? 256 : 64;
            value = valid;
            value[field] = std::string(limit, 'a');
            write(root / "plugin.json", value.dump());
            (void)dfy::plugins::load_manifest(root);
            value[field] = std::string(limit + 1, 'a');
            rejects(root, value.dump(), field);
        }

        for (const auto* field : {"manifest_version", "id", "name", "version", "abi_version", "entry"}) {
            value = valid;
            value.erase(field);
            rejects(root, value.dump(), field);
        }
        for (const auto* field : {"id", "name", "version", "entry", "default_config"}) {
            for (const Json& wrong : {Json(nullptr), Json(42), Json(""), Json(" "), Json("bad\ntext")}) {
                value = valid;
                value[field] = wrong;
                rejects(root, value.dump(), field);
            }
        }
        for (const auto* field : {"manifest_version", "abi_version"}) {
            for (const Json& wrong : {Json(0), Json(-1), Json(4294967296ULL), Json(1.0), Json(true), Json("1")}) {
                value = valid;
                value[field] = wrong;
                rejects(root, value.dump(), field);
            }
        }
        value = valid;
        value["manifest_version"] = 2;
        rejects(root, value.dump(), "manifest_version");
        for (const auto* id : {"../escape", "Example.Observer", "con", "nul.plugin", "a..b", "com1.plugin"}) {
            value = valid;
            value["id"] = id;
            rejects(root, value.dump(), "id:");
        }
        for (const auto* path : {"../outside.dll", "C:/outside.dll", "C:relative.dll", "/outside.dll",
                 "\\\\server\\share\\x.dll", "内容/../outside.dll", "内容/./入口.dll", "内容./入口.dll",
                 "NUL.dll", "COM¹.dll", "入口.dll:stream.dll", "missing.dll", "directory.dll", "bad.exe"}) {
            value = valid;
            value["entry"] = path;
            rejects(root, value.dump(), "entry:");
        }
        value = valid;
        value["default_config"] = "../outside.json";
        rejects(root, value.dump(), "default_config:");
        value["default_config"] = "absent.json";
        rejects(root, value.dump(), "default_config:");
        value = valid;
        value["unknown"] = "private-test-marker";
        rejects(root, value.dump(), "unknown field");
        rejects(root, "{\"id\":\"a\",\"id\":\"b\"}", "duplicate field");
        rejects(root, "[1,2]", "expected object");
        rejects(root, "{private-test-marker}", "invalid JSON");
        rejects(root, "{\"name\":\"" + std::string(1, static_cast<char>(0xFF)) + "\"}", "invalid JSON");
        rejects(root, std::string(64 * 1024 + 1, ' '), "64 KiB");

        fs::create_directory(run / "outside");
        write(run / "outside/outside.dll", "outside package");
        fs::create_symlink(fs::absolute(run / "outside/outside.dll"), root / "link.dll");
        fs::create_directory_symlink(fs::absolute(run / "outside"), root / "linked");
        for (const auto* entry : {"link.dll", "linked/outside.dll"}) {
            value = valid;
            value["entry"] = entry;
            rejects(root, value.dump(), "entry:");
        }
        value = valid;
        value["default_config"] = "link.dll";
        rejects(root, value.dump(), "default_config:");
        fs::create_directories(run / "linked-manifest");
        fs::create_directory(run / "empty-package");
        fs::create_symlink(fs::absolute(root / "plugin.json"), run / "linked-manifest/plugin.json");
        fs::create_directory_symlink(fs::absolute(root), run / "linked-package");
        for (const auto* package : {"linked-manifest", "linked-package", "absent-package", "empty-package"}) {
            bool rejected = false;
            try {
                (void)dfy::plugins::load_manifest(run / package);
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            check(rejected, "linked or absent package accepted");
        }
        std::cout << "Plugin manifest checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
