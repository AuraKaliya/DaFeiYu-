#include <dafeiyu/plugin_module_v2.hpp>
#include <windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using namespace dfy::plugins;
// 在加载器未正确拒绝或回收时报告失败。
void check(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
// 创建独立的真实 v2 目录包，沿用生产清单解析。
// 步骤：建立目录并复制 DLL → 写入清单 → 读取正式描述。
Manifest package(const fs::path& directory, const fs::path& dll) {
    fs::create_directories(directory);
    fs::copy_file(dll, directory / "entry.dll");
    std::ofstream file(directory / "plugin.json", std::ios::binary);
    file << R"({"manifest_version":1,"id":"test.v2","name":"v2","version":"1","abi_version":2,"entry":"entry.dll"})";
    file.close();
    check(static_cast<bool>(file), "cannot write manifest");
    return load_manifest(directory);
}
// 验证错误类别及模块错误正文隔离。
void rejects(const Manifest& manifest, const std::string& expected) {
    try { PluginModuleV2 module(manifest, 1); }
    catch (const std::exception& error) {
        const std::string message = error.what();
        check(message.find(expected) != std::string::npos && message.find("private-marker") == std::string::npos,
            "wrong v2 diagnostic");
        return;
    }
    throw std::runtime_error("invalid v2 module accepted");
}
}

// 使用真实 C DLL 验证生产 v2 引导查询与失败回收，保留 v1 查询路径。
// 步骤：建立包 → 逐项验证引导故障 → 验证前置拒绝及缺失导出 → 验证关闭与重新加载。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 4, "expected fault DLL, v1 DLL and work directory");
        fs::create_directories(argv[3]);
        fs::path run;
        for (unsigned i = 1;; ++i) {
            run = fs::path(argv[3]) / std::to_string(i);
            if (fs::create_directory(run)) break;
        }
        const auto manifest = package(run / L"中文 v2", fs::canonical(argv[1]));
        const char* errors[] = {"", "status 5", "invalid success", "table size", "table size", "ABI version",
            "architecture", "reserved", "required function", "error length", "invalid success", "status 4294967295"};
        for (uint32_t mode = 0; mode < 12; ++mode) {
            dfy::platform::DynamicLibrary control(manifest.entry);
            const auto select = reinterpret_cast<void (DFY_V2_CALL*)(uint32_t)>(control.symbol("dfy_fixture_mode"));
            const auto queries = reinterpret_cast<uint32_t (DFY_V2_CALL*)()>(control.symbol("dfy_fixture_queries"));
            select(mode);
            if (!mode) {
                PluginModuleV2 module(manifest, 1);
                check(module.open() && module.close() == dfy::platform::ModuleClose::closed && !module.open(), "close failed");
                check(module.close() == dfy::platform::ModuleClose::closed, "repeat close failed");
            } else rejects(manifest, errors[mode]);
            check(queries() == 1, "unexpected query count");
            control.close();
            check(!GetModuleHandleW(manifest.entry.c_str()), "query leaked module reference");
        }
        for (bool wrong_version : {false, true}) {
            auto stale = manifest;
            if (wrong_version) stale.abi_version = 1;
            else stale.name = "stale";
            dfy::platform::DynamicLibrary control(manifest.entry);
            const auto queries = reinterpret_cast<uint32_t (DFY_V2_CALL*)()>(control.symbol("dfy_fixture_queries"));
            rejects(stale, wrong_version ? "unsupported manifest ABI" : "manifest changed");
            check(!queries(), "preflight rejection called DLL");
        }
        const auto v1 = package(run / "v1", fs::canonical(argv[2]));
        rejects(v1, "export not found");
        check(!GetModuleHandleW(v1.entry.c_str()), "missing export leaked module");
        { PluginModuleV2 reloaded(manifest, 1); check(reloaded.open(), "reload failed"); }
        check(!GetModuleHandleW(manifest.entry.c_str()), "destructor leaked module");
        std::cout << "Plugin v2 bootstrap checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
