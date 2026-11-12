#include <dafeiyu/resource_owner.hpp>
#include <iostream>

namespace {
using namespace dfy::runtime;
// 在资源阶段或实例隔离违反契约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 验证非法状态操作拒绝，不把失败当作完成。
// 步骤：执行调用 → 捕获状态拒绝 → 对未拒绝的结果报告失败。
template<class Error, class Action>
void rejects(Action action) {
    try { action(); } catch (const Error&) { return; }
    throw std::runtime_error("resource: invalid transition accepted");
}

// 验证先登记、后建立、关闭接纳和失败清理的独立阶段。
// 步骤：登记并建立资源 → 停止接纳 → 保留清理失败 → 显式重试 → 确认所有者关闭。
void verify_cleanup() {
    ResourceOwner owner(2);
    rejects<std::logic_error>([&] { owner.finish_close(); });
    const auto active = owner.prepare(), partial = owner.prepare();
    check(owner.resource_state(active) == ResourceState::preparing && owner.size() == 2, "prepare did not track resource");
    rejects<ReferenceError>([&] { (void)owner.prepare(); });
    owner.activate(active);
    rejects<std::logic_error>([&] { owner.activate(active); });
    rejects<std::logic_error>([&] { owner.fail_cleanup(active); });
    rejects<std::logic_error>([&] { owner.finish_cleanup(active); });
    owner.stop_accepting();
    owner.stop_accepting();
    rejects<std::logic_error>([&] { (void)owner.prepare(); });
    rejects<std::logic_error>([&] { owner.finish_close(); });

    owner.begin_cleanup(partial);
    owner.fail_cleanup(partial);
    check(owner.resource_state(partial) == ResourceState::cleanup_failed && owner.size() == 2 &&
        owner.state() == OwnerState::closing, "failed cleanup lost responsibility");
    rejects<std::logic_error>([&] { owner.finish_cleanup(partial); });
    owner.begin_cleanup(partial);
    owner.begin_cleanup(partial);
    owner.finish_cleanup(partial);
    rejects<ReferenceError>([&] { owner.finish_cleanup(partial); });
    owner.begin_cleanup(active);
    owner.finish_cleanup(active);
    owner.finish_close();
    owner.finish_close();
    owner.stop_accepting();
    check(owner.state() == OwnerState::closed && !owner.size(), "owner close incomplete");
    rejects<std::logic_error>([&] { (void)owner.prepare(); });
}

// 验证资源引用绑定所有者实例及代次，复用容量不会复用旧身份。
// 步骤：创建独立所有者 → 拒绝跨域 → 复用资源槽 → 重建所有者并检查旧句柄。
void verify_identity() {
    Handle stale;
    DomainId old_domain = 0;
    {
        ResourceOwner first(1), second(1);
        stale = first.prepare();
        old_domain = first.domain();
        check(first.domain() != second.domain(), "owners shared issuing domain");
        rejects<ReferenceError>([&] { (void)second.resource_state(stale); });
        first.begin_cleanup(stale);
        first.finish_cleanup(stale);
        const auto reused = first.prepare();
        check(reused != stale && reused.id == stale.id, "resource generation not advanced");
        rejects<ReferenceError>([&] { first.activate(stale); });
        first.stop_accepting();
        first.activate(reused);
        check(first.state() == OwnerState::closing, "late initialization reopened admission");
        first.begin_cleanup(reused);
        first.finish_cleanup(reused);
        first.finish_close();
        second.stop_accepting();
        second.finish_close();
    }
    ResourceOwner replacement(1);
    check(replacement.domain() != old_domain, "owner identity reused");
    rejects<ReferenceError>([&] { replacement.begin_cleanup(stale); });
    replacement.stop_accepting();
    replacement.finish_close();
}
}

// 验收资源归属登记与清理进度，实际副作用、使用凭证及 DLL 依赖由后续单元管理。
// 步骤：验证关闭与重试 → 验证实例代次 → 汇报结果。
int main() {
    try {
        verify_cleanup();
        verify_identity();
        std::cout << "Runtime resource checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
