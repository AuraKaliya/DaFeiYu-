#include <dafeiyu/event_store.hpp>
#include "../events/sequence.hpp"
#include <iostream>

namespace {
using namespace dfy::runtime;
using namespace dfy::events;
// 在事件身份、接纳或引用寿命违反契约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

// 创建带明确字节约束的宿主测试载荷，保留完整整数精度。
std::shared_ptr<const StructValue> payload() {
    const FieldDescriptor field{"id", ValueKind::uint64, 0};
    auto descriptor = std::make_shared<const StructDescriptor>(StructDescriptor::create("app.event", 1, {&field, 1}, {1, 32}));
    const NamedValue value{"id", Value::from_uint64(UINT64_MAX)};
    return std::make_shared<const StructValue>(StructValue::create(descriptor, {&value, 1}, 8));
}

// 验证顺序接纳、独立保留、容量失败不推进以及事件与引用身份分离。
// 步骤：接纳并保留 → 用完引用容量 → 拒绝后回收一个名额 → 接纳下一事件并释放。
void verify_admission() {
    EventStore store(3, 8);
    auto data = payload();
    const std::weak_ptr<const StructValue> lifetime = data;
    const auto first = store.accept(data), retained = store.retain(first), second = store.accept(data);
    const auto first_id = store.get(first).id;
    check(first != retained && first_id == store.get(retained).id && first_id.domain == store.domain() &&
        first_id.sequence == 1 && store.get(second).id.sequence == 2 && !store.available_references(), "event identity conflated");
    bool full = false;
    try { (void)store.accept(data); } catch (const ReferenceError& error) { full = error.reason() == ReferenceFailure::exhausted; }
    check(full && store.references() == 3, "capacity refusal changed references");
    store.release(first);
    const auto third = store.accept(data);
    check(store.get(third).id.sequence == 3 && store.get(retained).id == first_id, "failed admission consumed sequence");
    data.reset();
    check(store.get(retained).payload->find("id")->as_uint64() == UINT64_MAX, "retained event lost payload");
    for (const auto handle : {retained, second, third}) store.release(handle);
    check(lifetime.expired() && store.available_references() == 3, "event or payload leaked");
    bool stale = false;
    try { (void)store.get(first); } catch (const ReferenceError& error) { stale = error.reason() == ReferenceFailure::invalid_handle; }
    check(stale, "released event reference accepted");
}

// 验证正文限制、空载荷拒绝和不同存储的接纳身份及句柄隔离。
// 步骤：拒绝不合法载荷 → 核对成功序号 → 检查独立域与跨表拒绝 → 核对序号末值算法。
void verify_boundaries() {
    EventStore small(1, 7), normal(1, 8);
    const auto data = payload();
    bool limited = false, invalid = false;
    try { (void)small.accept(data); } catch (const std::length_error&) { limited = true; }
    try { (void)normal.accept({}); } catch (const std::invalid_argument&) { invalid = true; }
    check(limited && invalid && !small.references() && !normal.references(), "failed admission leaked");
    const auto reference = normal.accept(data);
    check(normal.get(reference).id.sequence == 1 && normal.domain() != small.domain(), "admission domains reused");
    bool foreign = false;
    try { (void)small.get(reference); } catch (const ReferenceError&) { foreign = true; }
    check(foreign, "foreign event reference accepted");
    normal.release(reference);
    static_assert(dfy::events::detail::following_sequence(UINT64_MAX - 1) == UINT64_MAX);
    static_assert(dfy::events::detail::following_sequence(UINT64_MAX) == 0 && dfy::events::detail::following_sequence(0) == 0);
}
}

// 验收事件接纳与有界引用，本单元不广播或创建消费者任务。
// 步骤：验证顺序及寿命 → 验证拒绝与边界 → 汇报结果。
int main() {
    try {
        verify_admission();
        verify_boundaries();
        std::cout << "Event store checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
