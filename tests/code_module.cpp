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
// 在活动保护下调用真实 C DLL，并验证其依赖函数结果。
// 步骤：进入调用保护 → 查找并调用导出 → 返回宿主后释放保护。
int invoke(const CodeLease& lease) {
    auto call = lease.enter();
    return reinterpret_cast<int (*)()>(call.symbol("dfy_loader_value"))();
}

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

struct BusyContext { CodeModule* module; CodeLease* lease; bool protected_call = false; };
// 在原生调用栈内释放普通凭证并尝试关闭，所有异常留在宿主边界内。
// 步骤：归还代码凭证 → 检查活动计量 → 验证忙碌拒绝。
void check_busy(void* user) noexcept {
    auto& context = *static_cast<BusyContext*>(user);
    try {
        context.lease->reset();
        context.protected_call = !context.module->leases() && context.module->active_calls() == 1 &&
            context.module->close() == ModuleClose::busy;
    } catch (...) { context.protected_call = false; }
}

struct DropContext { std::unique_ptr<CodeModule>* module; CodeLease* lease; };
// 在真实调用中释放控制对象及普通凭证，活动保护必须独立保持代码直至返回。
void drop_holders(void* user) noexcept {
    auto& context = *static_cast<DropContext*>(user);
    context.lease->reset();
    context.module->reset();
}

// 验证活动容量、移动转交、忙碌关闭及控制方在原生调用期间退出的寿命。
// 步骤：进入并移动活动保护 → 在 DLL 回调中尝试关闭 → 返回后关闭 → 验证最后活动持有。
void verify_active_calls(const fs::path& file) {
    using Invoke = int (*)(void (*)(void*), void*);
    {
        CodeModule module(file, 1, 2);
        auto lease = module.lease();
        auto first = lease.enter(), second = lease.enter();
        check(module.active_calls() == 2, "configured active capacity ignored");
        first.reset();
        check(module.active_calls() == 1, "independent active scope lost");
        second.reset();
        try { auto failed = lease.enter(); (void)failed.symbol("dfy_no_such_export"); }
        catch (const std::runtime_error&) {}
        check(!module.active_calls(), "symbol failure leaked active scope");
        lease.reset();
        check(module.close() == ModuleClose::closed, "configured scope cleanup failed");
    }
    {
        CodeModule module(file, 1);
        auto lease = module.lease();
        auto entered = lease.enter();
        bool full = false;
        try { (void)lease.enter(); } catch (const std::length_error&) { full = true; }
        check(full && module.active_calls() == 1 && module.close() == ModuleClose::busy, "active capacity or busy guard failed");
        CodeCall moved(std::move(entered)), active;
        active = std::move(moved);
        active = std::move(active);
        BusyContext context{&module, &lease};
        check(reinterpret_cast<Invoke>(active.symbol("dfy_loader_invoke"))(check_busy, &context) == 8 && context.protected_call,
            "native active call lost module protection");
        active.reset();
        active.reset();
        check(!module.active_calls() && module.close() == ModuleClose::closed, "active guard did not finish");
    }
    auto module = std::make_unique<CodeModule>(file, 1);
    auto lease = module->lease();
    auto active = lease.enter();
    DropContext context{&module, &lease};
    check(reinterpret_cast<Invoke>(active.symbol("dfy_loader_invoke"))(drop_holders, &context) == 8 &&
        !module && GetModuleHandleW(file.c_str()), "last active holder did not keep native stack alive");
    active.reset();
    check(!GetModuleHandleW(file.c_str()), "final active holder leaked module");
}
}

// 使用真实 C DLL 验收代码保持及活动保护，不接入插件 ABI 或实例停止。
// 步骤：校验参数与容量 → 验证凭证及排队 → 验证原生活动调用 → 汇报结果。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 2, "expected payload DLL path");
        const auto file = fs::canonical(argv[1]);
        check(!GetModuleHandleW(file.c_str()), "fixture already loaded");
        bool zero = false;
        try { CodeModule invalid(file, 0); } catch (const std::invalid_argument&) { zero = true; }
        check(zero && !GetModuleHandleW(file.c_str()), "invalid capacity loaded DLL");
        zero = false;
        try { CodeModule invalid(file, 1, 0); } catch (const std::invalid_argument&) { zero = true; }
        check(zero && !GetModuleHandleW(file.c_str()), "zero call capacity loaded DLL");
        verify_leases(file);
        verify_queued_code(file);
        verify_active_calls(file);
        std::cout << "Code module checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
