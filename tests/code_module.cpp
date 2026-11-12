#include <dafeiyu/code_module.hpp>
#include <dafeiyu/fifo_executor.hpp>
#include <windows.h>
#include <iostream>
#include <stdexcept>

namespace {
using namespace dfy::platform;
namespace fs = std::filesystem;
// 在真实模块引用或代码持有行为违反契约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 在有效凭证覆盖下调用真实 C DLL，并验证其依赖函数结果。
int invoke(const CodeLease& lease) { return reinterpret_cast<int (*)()>(lease.symbol("dfy_loader_value"))(); }

// 验证显式保留、移动、容量及控制对象退出后的真实 DLL 寿命。
// 步骤：取得独立凭证 → 拒绝关闭和超限 → 移出最后凭证 → 退出控制对象后调用并最终释放。
void verify_leases(const fs::path& file) {
    CodeLease survivor;
    {
        CodeModule module(file, 2);
        auto first = module.lease();
        auto second = first.retain();
        check(module.leases() == 2 && invoke(first) == 8 && module.close() == ModuleClose::in_use, "live module closed");
        bool full = false;
        try { (void)module.lease(); } catch (const std::length_error&) { full = true; }
        check(full && module.leases() == 2, "lease capacity changed accounting");
        first.reset();
        first.reset();
        survivor = std::move(second);
        survivor = std::move(survivor);
        check(!second && survivor && module.leases() == 1, "move changed lease count");
    }
    check(GetModuleHandleW(file.c_str()) && invoke(survivor) == 8, "controller destruction unloaded leased code");
    survivor.reset();
    check(!GetModuleHandleW(file.c_str()), "last lease retained module");
    bool empty = false;
    try { (void)survivor.symbol("dfy_loader_value"); } catch (const std::logic_error&) { empty = true; }
    check(empty, "empty lease exposed code");
}

// 验证 FIFO 排队捕获保持代码，执行完归还凭证后才能显式关闭。
// 步骤：提交持有真实 DLL 的工作 → 释放客户端持有 → 推进调用 → 确认关闭及重新借用拒绝。
void verify_queued_code(const fs::path& file) {
    CodeModule module(file, 1);
    dfy::runtime::FifoExecutor executor(1);
    auto lease = std::make_shared<CodeLease>(module.lease());
    int result = 0;
    executor.post([lease, &result] { result = invoke(*lease); });
    lease.reset();
    check(module.close() == ModuleClose::in_use && module.leases() == 1 && result == 0, "queued code not kept");
    check(executor.run_one() == dfy::runtime::RunResult::executed && result == 8 && !module.leases(), "queued lease leaked");
    check(module.close() == ModuleClose::closed && module.close() == ModuleClose::closed && !GetModuleHandleW(file.c_str()),
        "explicit close did not release module");
    bool closed = false;
    try { (void)module.lease(); } catch (const std::logic_error&) { closed = true; }
    check(closed, "closed module leased again");
}
}

// 使用真实 C DLL 验收代码保持引用，不接入插件 ABI、活动调用计数或实例停止。
// 步骤：校验参数与零容量 → 验证凭证寿命 → 验证排队调用与关闭 → 汇报结果。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 2, "expected payload DLL path");
        const auto file = fs::canonical(argv[1]);
        check(!GetModuleHandleW(file.c_str()), "fixture already loaded");
        bool zero = false;
        try { CodeModule invalid(file, 0); } catch (const std::invalid_argument&) { zero = true; }
        check(zero && !GetModuleHandleW(file.c_str()), "invalid capacity loaded DLL");
        verify_leases(file);
        verify_queued_code(file);
        std::cout << "Code module checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
