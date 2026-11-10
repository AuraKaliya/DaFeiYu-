#include <dafeiyu/plugin_module.hpp>
#include <nlohmann/json.hpp>
#include <windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
namespace fs = std::filesystem;
using namespace dfy::plugins;

// 在 ABI 查询或模块引用行为不符合预期时报告失败。
void check(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

// 在隔离目录创建包含真实 DLL 的最小包并返回正式清单描述。
// 步骤：复制 DLL → 写入清单 → 使用正式解析器读取。
Manifest package(const fs::path& directory, const fs::path& dll, unsigned abi = DFY_ABI_VERSION) {
    fs::create_directories(directory);
    fs::copy_file(dll, directory / "entry.dll");
    std::ofstream file(directory / "plugin.json", std::ios::binary);
    file << nlohmann::json{{"manifest_version", 1}, {"id", "test.module"}, {"name", "查询测试"},
        {"version", "1"}, {"abi_version", abi}, {"entry", "entry.dll"}}.dump();
    file.close();
    check(static_cast<bool>(file), "cannot write manifest");
    return load_manifest(directory);
}

// 验证失败类别，不允许错误间接回显模块提供的敏感正文。
// 步骤：尝试构造 → 捕获错误并核对类别及正文隔离。
void rejects(const Manifest& manifest, const std::string& expected) {
    bool rejected = false;
    try { PluginModule module(manifest); }
    catch (const std::exception& error) {
        const std::string message = error.what();
        rejected = message.find(expected) != std::string::npos && message.find("private-test-marker") == std::string::npos;
    }
    check(rejected, "query failure accepted or wrong diagnostic");
}
}

// 用真实 DLL 验证兼容性校验、未调用生命周期和失败后的独立引用回收。
// 步骤：创建真实包 → 验证成功持有与关闭 → 逐项拒绝故障结果 → 检查前置校验和缺失导出。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 5, "expected query fixture, example, no-export DLL and work directory");
        const fs::path fixture = fs::canonical(argv[1]), example = fs::canonical(argv[2]), no_export = fs::canonical(argv[3]);
        fs::create_directories(argv[4]);
        fs::path run;
        for (unsigned i = 1; ; ++i) {
            run = fs::path(argv[4]) / std::to_string(i);
            if (fs::create_directory(run)) break;
        }
        const auto manifest = package(run / L"中文 查询包", fixture);
        const auto sample = package(run / "example", example);
        {
            PluginModule module(sample);
            check(module.api().abi_version == DFY_ABI_VERSION && module.api().create && GetModuleHandleW(sample.entry.c_str()),
                "example query or module ownership failed");
            module.close();
            module.close();
            bool closed = false;
            try { (void)module.api(); } catch (const std::runtime_error&) { closed = true; }
            check(closed && !GetModuleHandleW(sample.entry.c_str()), "closed module retained interface or reference");
        }
        {
            PluginModule module(sample);
            check(module.api().struct_size == sizeof(dfy_plugin_api), "reload failed");
        }
        check(!GetModuleHandleW(sample.entry.c_str()), "destructor leaked reference");

        const std::pair<uint32_t, const char*> cases[] = {{0, ""}, {1, "failed status 6"}, {2, "invalid success"},
            {3, "table size"}, {4, "table size"}, {5, "ABI version"}, {6, "architecture"}, {7, "reserved"},
            {8, "required function"}, {9, "required function"}, {10, "required function"}, {11, "required function"},
            {12, "error length"}, {13, "invalid success"}, {14, "failed status 6"}, {15, "status 4294967295"}};
        for (const auto& [mode, expected] : cases) {
            dfy::platform::DynamicLibrary control(manifest.entry);
            const auto select = reinterpret_cast<void (DFY_CALL*)(uint32_t)>(control.symbol("dfy_fixture_mode"));
            const auto queries = reinterpret_cast<uint32_t (DFY_CALL*)()>(control.symbol("dfy_fixture_queries"));
            const auto lifecycle = reinterpret_cast<uint32_t (DFY_CALL*)()>(control.symbol("dfy_fixture_lifecycle_calls"));
            select(mode);
            if (mode == 0) { PluginModule module(manifest); check(module.api().destroy != nullptr, "valid table rejected"); }
            else { rejects(manifest, expected); }
            check(queries() == 1 && lifecycle() == 0, "query count or lifecycle scope incorrect");
            // 控制端持有另一份引用；被测构造失败不能偷走它，也不能留下多余引用。
            control.close();
            check(!GetModuleHandleW(manifest.entry.c_str()), "query failure leaked module reference");
        }

        for (auto invalid : {manifest, package(run / "future", fixture, 2)}) {
            const bool unsupported = invalid.abi_version != DFY_ABI_VERSION;
            if (!unsupported) invalid.name = "stale description";
            dfy::platform::DynamicLibrary control(invalid.entry);
            const auto queries = reinterpret_cast<uint32_t (DFY_CALL*)()>(control.symbol("dfy_fixture_queries"));
            rejects(invalid, unsupported ? "unsupported manifest ABI" : "manifest changed");
            check(queries() == 0, "preflight rejection still queried DLL");
            control.close();
            check(!GetModuleHandleW(invalid.entry.c_str()), "preflight rejection retained reference");
        }
        const auto missing_export = package(run / "missing-export", no_export);
        rejects(missing_export, "export not found");
        check(!GetModuleHandleW(missing_export.entry.c_str()), "missing export leaked module reference");
        std::cout << "Plugin module query checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
