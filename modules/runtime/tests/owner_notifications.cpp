#include <dafeiyu/completion_notifications.hpp>
#include <iostream>
namespace {
using namespace dfy::runtime;
// 在通知的公平调度、撤销或活动保护违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
}
// 验证同一通知责任可进入所有者调度，背压、失败和撤销均保持原通知语义。
// 步骤：持续登记不同所有者通知 → 验证加权和延后执行 → 核对控制预留 → 撤销排队通知并回收。
int main() {
    try {
        OwnerScheduler scheduler(2); const auto a = scheduler.add_owner(3,4,1), b = scheduler.add_owner(1,4,1);
        CompletionNotifications first(5), second(4); OperationSource done; done.settle(OperationState::succeeded);
        std::vector<int> trace; std::vector<Handle> registrations, others;
        for (int i = 0; i < 4; ++i) {
            registrations.push_back(first.subscribe(done.operation(),[&](const auto&) { check(first.running(),"notification lost activity"); trace.push_back(1); }));
            others.push_back(second.subscribe(done.operation(),[&](const auto&) { trace.push_back(2); }));
            check(first.schedule_one(scheduler,a) == ScheduleResult::queued && second.schedule_one(scheduler,b) == ScheduleResult::queued,"owner notify enqueue failed");
        }
        const auto control = first.subscribe(done.operation(),[&](const auto&) { trace.push_back(3); throw std::runtime_error("isolated"); });
        check(trace.empty() && first.schedule_one(scheduler,a) == ScheduleResult::blocked &&
            first.schedule_one(scheduler,a,TaskPriority::control) == ScheduleResult::queued,"notification bypassed backpressure or control reserve failed");
        check(scheduler.run_one() == RunResult::failed && trace == std::vector<int>{3} && first.read(control) == NotificationState::delivered,
            "control notification exception escaped or did not settle");
        for (int i = 0; i < 4; ++i) check(scheduler.run_one() == RunResult::executed,"weighted notification failed");
        check(trace == std::vector<int>({3,1,1,1,2}),"notification owner weight changed");
        first.revoke(registrations.back()); second.revoke_pending();
        while (scheduler.run_one() != RunResult::idle) {}
        check(trace.size() == 5,"revoked queued notification entered consumer");
        for (const auto handle : registrations) first.release(handle);
        for (const auto handle : others) second.release(handle);
        first.release(control); first.collect(); second.collect(); check(!first.size() && !second.size(),"owner notification wrapper leaked");
        scheduler.stop(a); scheduler.stop(b); scheduler.remove(a); scheduler.remove(b);
        std::cout << "Owner notification checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
