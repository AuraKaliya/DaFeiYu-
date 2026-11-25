#include <dafeiyu/handle_registry.hpp>
#include <iostream>
#include <string>

namespace {
using namespace dfy::runtime;
// 在生产引用表不满足契约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 验证操作以指定引用错误拒绝，不把其他异常误认为预期结果。
template<class Action>
void rejected(ReferenceFailure reason, Action action) {
    try { action(); }
    catch (const ReferenceError& error) { check(error.reason() == reason, "wrong reference failure"); return; }
    throw std::runtime_error("reference operation unexpectedly succeeded");
}
}

// 验证真实类型化引用表的身份、共享、容量与回收边界。
// 步骤：构造独立表 → 检查独立持有与容量 → 拒绝失效身份 → 检查共享限制及最终回收。
int main() {
    try {
        using namespace dfy::runtime;
        HandleRegistry<std::string> registry(2), another(2);
        check(registry.domain() != another.domain(), "domains collided");
        auto value = std::make_shared<std::string>("host-owned");
        const std::weak_ptr<std::string> lifetime = value;
        const auto original = registry.insert(value);
        const auto retained = registry.retain(original);
        check(registry.contains(original) && registry.contains(retained) && !another.contains(original), "reference presence query wrong");
        value.reset();
        check(original != retained && registry.size() == 2 && &registry.get(original) == &registry.get(retained),
            "independent reference copied the object");
        rejected(ReferenceFailure::exhausted, [&] { (void)registry.retain(original); });
        check(registry.size() == 2 && registry.get(original) == "host-owned", "capacity failure changed references");
        registry.release(original);
        check(!lifetime.expired() && registry.get(retained) == "host-owned", "release destroyed another reference");
        const auto reused = registry.retain(retained);
        check(reused.id == original.id && reused.generation != original.generation, "slot identity reused");
        for (auto invalid : {Handle{}, original, Handle{another.domain(), retained.id, retained.generation},
            Handle{registry.domain(), 0, 1}, Handle{registry.domain(), 3, 1}, Handle{registry.domain(), retained.id, 0}}) {
            check(!registry.contains(invalid), "invalid reference reported present");
            rejected(ReferenceFailure::invalid_handle, [&] { (void)registry.get(invalid); });
            rejected(ReferenceFailure::invalid_handle, [&] { (void)registry.retain(invalid); });
            if (!invalid.empty()) rejected(ReferenceFailure::invalid_handle, [&] { registry.release(invalid); });
        }
        registry.release({});
        registry.release(reused);
        rejected(ReferenceFailure::invalid_handle, [&] { registry.release(reused); });
        registry.release(retained);
        check(lifetime.expired() && registry.size() == 0, "last reference leaked");

        const auto exclusive = registry.insert(std::make_shared<std::string>("exclusive"), false);
        rejected(ReferenceFailure::not_shareable, [&] { (void)registry.retain(exclusive); });
        check(registry.size() == 1 && registry.get(exclusive) == "exclusive", "nonshareable rejection changed source");
        registry.release(exclusive);
        bool null_rejected = false;
        try { (void)registry.insert({}); } catch (const std::invalid_argument&) { null_rejected = true; }
        check(null_rejected && registry.size() == 0, "null object inserted");
        bool empty_rejected = false;
        try { HandleRegistry<int> empty(0); } catch (const std::invalid_argument&) { empty_rejected = true; }
        check(empty_rejected, "zero capacity accepted");
        const auto before_clear = registry.insert(std::make_shared<std::string>("clear"));
        registry.clear(); registry.clear();
        check(!registry.size(), "clear retained references");
        rejected(ReferenceFailure::invalid_handle, [&] { (void)registry.get(before_clear); });
        const auto after_clear = registry.insert(std::make_shared<std::string>("new"));
        check(after_clear != before_clear, "clear reused reference identity");
        registry.release(after_clear);

        Handle old{};
        std::weak_ptr<int> destroyed;
        {
            HandleRegistry<int> previous(1);
            const auto object = std::make_shared<int>(7);
            destroyed = object;
            old = previous.insert(object);
        }
        HandleRegistry<int> replacement(1);
        check(destroyed.expired() && replacement.domain() != old.domain, "destroyed registry retained identity or data");
        rejected(ReferenceFailure::invalid_handle, [&] { (void)replacement.get(old); });
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        check(detail::following_generation(maximum - 1) == maximum && detail::following_generation(maximum) == 0 &&
            detail::following_generation(0) == 0, "exhausted generation reused");
        std::cout << "Runtime handle registry checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
