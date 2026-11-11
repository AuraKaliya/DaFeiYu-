#include <dafeiyu/events.hpp>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using namespace dfy::events;
const Event event{Kind::turn_started, 17, 3};

// 在通知行为不符合契约时报告测试失败。
void check(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}

// 检查订阅顺序、取消返回值、事件数据和独立总线。
// 步骤：验证空总线与非法订阅 → 注册并通知 → 分别取消订阅及所有者。
void basic() {
    Bus bus, other;
    auto report = bus.publish(event);
    check(report.status == PublishStatus::completed && report.succeeded == 0 && report.failed == 0 &&
              report.first_failed_subscription == 0 && report.first_failed_owner == 0,
        "empty publish failed");
    for (OwnerId owner : {OwnerId{0}, OwnerId{1}}) {
        bool rejected = false;
        try {
            bus.subscribe(owner, owner == 0 ? Bus::Callback([](const Event&) {}) : Bus::Callback{});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "invalid subscription accepted");
    }

    std::vector<int> seen;
    const auto first = bus.subscribe(10, [&](const Event& value) {
        check(value.kind == event.kind && value.session_id == 17 && value.turn_id == 3, "event changed");
        seen.push_back(1);
    });
    bus.subscribe(20, [&, count = 0](const Event&) mutable { seen.push_back(20 + ++count); });
    bus.subscribe(10, [&](const Event&) { seen.push_back(3); });
    report = bus.publish(event);
    check(report.succeeded == 3 && report.failed == 0 && seen == std::vector<int>{1, 21, 3}, "order changed");
    check(other.publish(event).succeeded == 0, "buses share subscriptions");

    check(bus.unsubscribe(first) && !bus.unsubscribe(first) && !bus.unsubscribe(0), "cancel result incorrect");
    check(bus.unsubscribe_owner(10) == 1 && bus.unsubscribe_owner(10) == 0, "owner cancellation incorrect");
    seen.clear();
    report = bus.publish(event);
    check(report.succeeded == 1 && report.failed == 0 && seen == std::vector<int>{22}, "cancel or state lost");
    check(bus.unsubscribe_owner(20) == 1 && bus.publish(event).succeeded == 0, "owner cleanup incomplete");
}

// 检查分发中的增删，以及自取消回调捕获对象的释放时机。
// 步骤：注册变更回调和其他所有者 → 发布时增删订阅 → 检查下一次发布与资源回收。
void mutations() {
    Bus bus;
    std::vector<int> seen;
    auto state = std::make_shared<int>(7);
    const std::weak_ptr<int> lifetime = state;
    SubscriptionId self = 0;
    // 执行自取消、其他所有者撤销和新增订阅，验证本次通知边界。
    // 步骤：取消已有订阅并检查存活 → 新增下轮订阅 → 取消尚未生效的订阅。
    self = bus.subscribe(10, [&, state](const Event&) {
        seen.push_back(*state);
        check(bus.unsubscribe(self) && !lifetime.expired(), "self cancellation destroyed callback");
        check(bus.unsubscribe_owner(20) == 2, "pending owner subscriptions not cancelled");

        const auto added = bus.subscribe(40, [&](const Event&) { seen.push_back(5); });
        check(added > self && !bus.unsubscribe(self), "subscription identifier reused");
        const auto cancelled = bus.subscribe(40, [&](const Event&) { seen.push_back(6); });
        check(bus.unsubscribe(cancelled), "new subscription cannot be cancelled");
    });
    state.reset();
    bus.subscribe(20, [&](const Event&) { seen.push_back(2); });
    bus.subscribe(20, [&](const Event&) { seen.push_back(3); });
    bus.subscribe(30, [&](const Event&) { seen.push_back(4); });

    auto report = bus.publish(event);
    check(report.succeeded == 2 && report.failed == 0 && seen == std::vector<int>{7, 4}, "dispatch mutation failed");
    check(lifetime.expired(), "cancelled callback retained after publish");
    seen.clear();
    report = bus.publish(event);
    check(report.succeeded == 2 && report.failed == 0 && seen == std::vector<int>{4, 5}, "next publish incorrect");
}

// 检查不同异常的隔离、失败归属、嵌套拒绝和错误后的继续使用。
// 步骤：注册失败和重入回调 → 核对分发报告 → 取消失败回调后再次发布。
void failures() {
    Bus bus;
    const auto first = bus.subscribe(10, [](const Event&) { throw std::runtime_error("callback failure"); });
    bus.subscribe(10, [](const Event&) { throw 42; });
    PublishResult nested;
    bus.subscribe(20, [&](const Event& value) { nested = bus.publish(value); });
    int calls = 0;
    bus.subscribe(20, [&](const Event&) { ++calls; });

    const auto report = bus.publish(event);
    check(report.status == PublishStatus::completed && report.succeeded == 2 && report.failed == 2 &&
              report.first_failed_subscription == first && report.first_failed_owner == 10 && calls == 1,
        "callback failures interrupted dispatch or lost attribution");
    check(nested.status == PublishStatus::reentrant && nested.succeeded == 0 && nested.failed == 0 &&
              nested.first_failed_subscription == 0 && nested.first_failed_owner == 0,
        "nested publish was dispatched");

    check(bus.unsubscribe_owner(10) == 2, "failed subscriptions cannot be removed");
    const auto recovered = bus.publish(event);
    check(recovered.status == PublishStatus::completed && recovered.succeeded == 2 && recovered.failed == 0 &&
              recovered.first_failed_subscription == 0 && recovered.first_failed_owner == 0 && calls == 2,
        "bus did not recover after failures");
}
}

// 运行独立事件契约测试，将失败转换为非零退出状态。
// 步骤：验证基本通知 → 验证分发中变更 → 验证异常与重入。
int main() {
    try {
        basic();
        mutations();
        failures();
        std::cout << "Event bus checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
