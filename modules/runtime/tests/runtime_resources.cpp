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

// 验证使用凭证绑定双方实例，容量拒绝和清理阻挡不改变现有关系。
// 步骤：申请并检查两侧容量 → 阻止仍在使用的清理 → 移动和释放凭证 → 逐个完成关闭。
void verify_uses() {
    ResourceOwner provider(1, 2), consumer(1, 1), another(1, 2);
    const auto resource = provider.prepare();
    rejects<std::logic_error>([&] { (void)provider.acquire(resource, consumer); });
    provider.activate(resource);
    auto first = provider.acquire(resource, consumer);
    check(first.resource() == resource && first.provider_domain() == provider.domain() && first.consumer_domain() == consumer.domain(),
        "use binding did not identify instances");
    rejects<std::length_error>([&] { (void)provider.acquire(resource, consumer); });
    auto second = provider.acquire(resource, another);
    rejects<std::length_error>([&] { (void)provider.acquire(resource, another); });
    check(provider.incoming_uses() == 2 && provider.resource_users(resource) == 2 && consumer.outgoing_uses() == 1,
        "rejected use changed accounting");
    rejects<std::logic_error>([&] { provider.begin_cleanup(resource); });
    check(provider.resource_state(resource) == ResourceState::active, "busy cleanup changed phase");
    provider.stop_accepting();
    consumer.stop_accepting();
    rejects<std::logic_error>([&] { (void)provider.acquire(resource, another); });
    rejects<std::logic_error>([&] { consumer.finish_close(); });
    ResourceUse moved(std::move(first)), assigned;
    assigned = std::move(moved);
    assigned = std::move(assigned);
    check(!first && !moved && assigned && provider.resource_users(resource) == 2, "move duplicated use");
    assigned.reset();
    assigned.reset();
    consumer.finish_close();
    check(provider.resource_users(resource) == 1 && !consumer.outgoing_uses(), "use release changed other consumer");
    second.reset();
    provider.begin_cleanup(resource);
    provider.finish_cleanup(resource);
    provider.finish_close();
    another.stop_accepting();
    another.finish_close();
    check(!provider.incoming_uses() && !another.outgoing_uses(), "use accounting leaked");
}

// 验证自用计量、覆盖移动和具体代次绑定，所有者停止后旧凭证仍有效直到释放。
// 步骤：建立自用及独立绑定 → 覆盖旧凭证 → 关闭消费者 → 回收并复用资源槽。
void verify_use_lifetime() {
    ResourceOwner disabled(1);
    const auto denied = disabled.prepare();
    disabled.activate(denied);
    rejects<std::length_error>([&] { (void)disabled.acquire(denied, disabled); });
    disabled.begin_cleanup(denied);
    disabled.finish_cleanup(denied);
    disabled.stop_accepting();
    disabled.finish_close();

    ResourceOwner provider(2, 3), consumer(1, 2), replacement(1, 1);
    const auto first = provider.prepare(), second = provider.prepare();
    provider.activate(first);
    provider.activate(second);
    auto use = provider.acquire(first, consumer);
    {
        auto self = provider.acquire(second, provider);
        check(provider.incoming_uses() == 2 && provider.outgoing_uses() == 1, "self use not counted");
    }
    auto newer = provider.acquire(second, replacement);
    const auto old_consumer = use.consumer_domain();
    consumer.stop_accepting();
    rejects<std::logic_error>([&] { (void)provider.acquire(first, consumer); });
    check(use.consumer_domain() == old_consumer && old_consumer != newer.consumer_domain(), "old binding migrated");
    use = std::move(newer);
    consumer.finish_close();
    check(provider.resource_users(first) == 0 && provider.resource_users(second) == 1 && !provider.outgoing_uses(),
        "overwritten use not released");
    provider.begin_cleanup(first);
    provider.finish_cleanup(first);
    const auto reused = provider.prepare();
    provider.activate(reused);
    rejects<ReferenceError>([&] { (void)provider.acquire(first, replacement); });
    use.reset();
    for (const auto item : {reused, second}) { provider.begin_cleanup(item); provider.finish_cleanup(item); }
    provider.stop_accepting();
    provider.finish_close();
    replacement.stop_accepting();
    replacement.finish_close();
}
}

// 验收资源归属、使用凭证及清理进度，实际副作用和 DLL 依赖由后续单元管理。
// 步骤：验证关闭与重试 → 验证实例代次 → 验证使用关系及计量 → 汇报结果。
int main() {
    try {
        verify_cleanup();
        verify_identity();
        verify_uses();
        verify_use_lifetime();
        std::cout << "Runtime resource checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
