#include <dafeiyu/subscription_queue.hpp>
#include <iostream>

namespace {
using namespace dfy::events;
using namespace dfy::runtime;
// 在交付顺序、关闭或引用寿命违反契约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 创建无需正文预算的合法宿主结构载荷。
std::shared_ptr<const StructValue> payload() {
    auto descriptor = std::make_shared<const StructDescriptor>(StructDescriptor::create("app.event", 1, {}, {0, 32}));
    return std::make_shared<const StructValue>(StructValue::create(descriptor, {}, 0));
}

// 验证空队列、FIFO、满队列拒绝和排空后已交付数据仍有效。
// 步骤：填充有界队列 → 拒绝超限 → 排空关闭 → 保留读出数据 → 最终归还全部引用。
void verify_drain() {
    auto store = std::make_shared<EventStore>(5, 0);
    SubscriptionQueue queue(store, 2);
    check(queue.poll().state == PollState::empty && !queue.close_mode(), "empty queue treated as closed");
    const auto first = store->accept(payload()), second = store->accept(payload());
    queue.push(first);
    queue.push(second);
    bool full = false;
    try { queue.push(first); } catch (const std::length_error&) { full = true; }
    check(full && store->references() == 4 && queue.size() == 2, "full queue changed references");
    store->release(first);
    store->release(second);
    queue.close(CloseMode::drain);
    bool closed = false;
    try { queue.push({}); } catch (const std::logic_error&) { closed = true; }
    auto a = queue.poll(), b = queue.poll();
    check(closed && a.state == PollState::event && b.state == PollState::event &&
        a.event.get().id.sequence == 1 && b.event.get().id.sequence == 2 && queue.poll().state == PollState::closed,
        "drain lost delivery order");
    auto retained = a.event.retain();
    a.event.reset();
    b.event.reset();
    check(retained.get().id.sequence == 1 && store->references() == 1, "drain invalidated delivered data");
    retained.reset();
    queue.close(CloseMode::cancel);
    check(!store->references() && queue.close_mode() == CloseMode::drain, "completed close reason rewritten");
}

// 验证排空升级取消、独立队列、存储背压及队列退出后的读出持有。
// 步骤：向两个接收方交付 → 取消一个接收方 → 转交另一个结果 → 销毁队列和外部存储持有。
void verify_cancel_and_lifetime() {
    EventLease retained;
    std::weak_ptr<EventStore> lifetime;
    {
        auto store = std::make_shared<EventStore>(3, 0);
        lifetime = store;
        SubscriptionQueue first(store, 2), second(store, 1);
        const auto event = store->accept(payload());
        first.push(event);
        second.push(event);
        bool limited = false;
        try { first.push(event); } catch (const ReferenceError& error) { limited = error.reason() == ReferenceFailure::exhausted; }
        check(limited && first.size() == 1, "reference pressure changed queue");
        first.close(CloseMode::drain);
        first.close(CloseMode::cancel);
        check(first.poll().state == PollState::closed && first.close_mode() == CloseMode::cancel && second.size() == 1,
            "cancellation affected other receiver");
        retained = std::move(second.poll().event);
        store->release(event);
    }
    check(!lifetime.expired() && retained.get().id.sequence == 1, "queue exit invalidated delivered data");
    EventLease moved(std::move(retained));
    moved = std::move(moved);
    check(!retained && moved, "lease move duplicated data ownership");
    moved.reset();
    check(lifetime.expired(), "delivered lease leaked store");
}
}

// 验收独立有界交付及两种关闭，不把流结束当作消费任务已经退出。
// 步骤：验证排空 → 验证取消及寿命 → 汇报结果。
int main() {
    try {
        verify_drain();
        verify_cancel_and_lifetime();
        std::cout << "Subscription queue checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
