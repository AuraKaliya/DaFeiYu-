#include <dafeiyu/plugin_module_v2.hpp>
#include <windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using namespace dfy::plugins;
struct TestApi { dfy_v2_api_header header; uint32_t (DFY_V2_CALL *value)(); };
struct Closing { PluginModuleV2* module; dfy::platform::ModuleClose result; };
// 在真实 DLL 栈内请求关闭，只记录结果，不抛出跨 ABI 异常。
void DFY_V2_CALL close_during_call(void* user) {
    auto& closing = *static_cast<Closing*>(user);
    closing.result = closing.module->close();
}
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

// 验证能力前缀、代码持有与活动调用，控制对象退出不破坏独立能力。
// 步骤：拒绝畸形能力 → 验证查询及原生调用保护 → 验证持有与控制器分离 → 回收全部引用。
void capabilities(const Manifest& manifest) {
    dfy::platform::DynamicLibrary control(manifest.entry);
    const auto select = reinterpret_cast<void (DFY_V2_CALL*)(uint32_t)>(control.symbol("dfy_fixture_mode"));
    const auto observe = reinterpret_cast<void (DFY_V2_CALL*)(void (DFY_V2_CALL*)(void*), void*)>(control.symbol("dfy_fixture_observer"));
    std::optional<Capability<TestApi>> surviving;
    {
        select(0);
        PluginModuleV2 module(manifest, 3);
        check(!module.query<TestApi>("unknown", 1) && !module.query<TestApi>("test.capability", 2), "unknown capability accepted");
        bool invalid = false;
        try { (void)module.query<TestApi>("", 1); } catch (const std::invalid_argument&) { invalid = true; }
        check(invalid, "empty capability name accepted");
        invalid = false;
        try { (void)module.query<TestApi>("test.capability", 0); } catch (const std::invalid_argument&) { invalid = true; }
        check(invalid, "zero capability version accepted");
        for (uint32_t mode = 20; mode <= 27; ++mode) {
            select(mode);
            bool rejected = false;
            try { const auto result = module.query<TestApi>("test.capability", 1); rejected = mode == 20 && !result; }
            catch (const std::runtime_error&) { rejected = mode != 20; }
            check(rejected, "invalid capability result accepted");
        }
        select(0);
        Closing closing{&module, dfy::platform::ModuleClose::closed};
        observe(close_during_call, &closing);
        surviving = module.query<TestApi>("test.capability", 1);
        check(surviving && closing.result == dfy::platform::ModuleClose::busy && module.open(), "query not protected");
        check(module.close() == dfy::platform::ModuleClose::in_use, "capability did not keep code");
        {
            auto call = surviving->enter();
            check(call.api().value && call.api().value() == 42 && closing.result == dfy::platform::ModuleClose::busy,
                "capability call not protected");
            auto moved = std::move(call);
            invalid = false;
            try { (void)call.api(); } catch (const std::logic_error&) { invalid = true; }
            check(invalid && moved.api().value() == 42, "moved call exposed unprotected table");
        }
        observe(nullptr, nullptr);
    }
    control.close();
    check(GetModuleHandleW(manifest.entry.c_str()) != nullptr, "controller exit invalidated capability");
    {
        auto duplicate = surviving->retain();
        auto call = duplicate.enter();
        surviving.reset();
        check(call.api().value() == 42, "independent capability lost code");
    }
    check(!GetModuleHandleW(manifest.entry.c_str()), "capability reference leaked");
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
        capabilities(manifest);
        std::cout << "Plugin v2 bootstrap checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
