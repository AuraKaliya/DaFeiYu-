#include <dafeiyu/dynamic_library.hpp>
#include <windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using dfy::platform::DynamicLibrary;

// 在真实 DLL 加载或引用管理行为不符合预期时报告失败。
void check(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}

// 验证失败原因，并在异常退出后确认本次没有留下模块引用。
// 步骤：尝试加载及查找 → 核对诊断 → 检查模块是否仍被持有。
void rejects(const fs::path& path, const std::string& expected, const std::string& symbol = "") {
    bool rejected = false;
    try {
        DynamicLibrary library(path);
        if (!symbol.empty()) {
            (void)library.symbol(symbol);
        }
    } catch (const std::exception& error) {
        rejected = std::string(error.what()).find(expected) != std::string::npos;
    }
    check(rejected, "unexpected library failure result");
    check(!GetModuleHandleW(path.c_str()), "failed operation leaked module reference");
}
}

// 使用隔离目录内的真实 DLL 验证引用、受控搜索和常见失败路径。
// 步骤：复制中文路径夹具 → 检查引用生命周期 → 检查查找与架构错误 → 验证依赖搜索范围。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 5, "expected native, dependent, x86 DLL and work paths");
        const fs::path dependency = fs::canonical(argv[1]), payload = fs::canonical(argv[2]), x86 = fs::canonical(argv[3]);
        const auto work = fs::absolute(argv[4]);
        fs::create_directories(work);
        fs::path run;
        for (unsigned i = 1; ; ++i) {
            run = work / std::to_string(i);
            if (fs::create_directory(run)) {
                break;
            }
        }
        const auto package = run / L"中文 插件";
        const auto outside = run / "outside";
        fs::create_directories(package);
        fs::create_directory(outside);
        const auto direct = package / L"直接 加载.dll";
        fs::copy_file(dependency, direct);
        const auto original_mode = GetThreadErrorMode();
        std::cout << "Checking module ownership\n" << std::flush;
        {
            DynamicLibrary first(direct), second(direct);
            const auto value = reinterpret_cast<int (*)()>(first.symbol("dfy_loader_dependency_value"));
            check(value() == 7, "wrong DLL export called");
            first.close();
            first.close();
            check(GetModuleHandleW(direct.c_str()) != nullptr, "close released another owner's reference");
            bool closed = false;
            try { (void)first.symbol("dfy_loader_dependency_value"); }
            catch (const std::runtime_error&) { closed = true; }
            check(closed && reinterpret_cast<int (*)()>(second.symbol("dfy_loader_dependency_value"))() == 7,
                "closed owner remained usable or second owner broken");
        }
        check(!GetModuleHandleW(direct.c_str()), "destructor retained module reference");
        std::cout << "Checking failure paths\n" << std::flush;
        rejects(direct, "export not found", "dfy_missing_export");
        rejects(package / "missing.dll", "file unavailable");
        rejects("relative.dll", "absolute .dll path");
        rejects(fs::path(direct.native() + std::wstring(1, L'\0') + L"ignored.dll"), "without NUL");
        const auto wrong_arch = package / "x86.dll";
        fs::copy_file(x86, wrong_arch);
        rejects(wrong_arch, "architecture");
        const auto invalid = package / "invalid.dll";
        { std::ofstream file(invalid); file << "not a PE image"; }
        rejects(invalid, "invalid image");

        std::cout << "Checking dependency search\n" << std::flush;
        const auto dependent = package / payload.filename();
        const auto private_dependency = package / dependency.filename();
        fs::copy_file(payload, dependent);
        fs::copy_file(dependency, outside / dependency.filename());
        const auto previous = fs::current_path();
        fs::current_path(outside);
        rejects(dependent, "dependency not found");
        fs::current_path(previous);
        fs::copy_file(dependency, private_dependency);
        {
            DynamicLibrary library(dependent);
            check(reinterpret_cast<int (*)()>(library.symbol("dfy_loader_value"))() == 8,
                "adjacent dependency not loaded");
        }
        check(!GetModuleHandleW(dependent.c_str()) && !GetModuleHandleW(private_dependency.c_str()),
            "dependency references survived release");
        check(GetThreadErrorMode() == original_mode, "thread error mode was not restored");
        std::cout << "Dynamic library checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
