#include <dafeiyu/owner_scheduler.hpp>
#include <iostream>
namespace {
using namespace dfy::runtime;
// 在资格、容量或所有者公平违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 核对拒绝类型，不吞没无关异常。
template<class Error, class Action>
void rejects(Action action) { try { action(); } catch (const Error&) { return; } throw std::runtime_error("scheduler accepted invalid action"); }
// 验证持续竞争的权重份额，活动步骤仍占容量，空闲所有者不积攒额度。
// 步骤：建立两个持续就绪所有者 → 推进固定步骤数 → 核对份额及活动容量 → 排空并移除。
void fairness() {
    OwnerScheduler scheduler(2); const auto a = scheduler.add_owner(3,2), b = scheduler.add_owner(1,2);
    unsigned first = 0, second = 0; bool refill = true;
    FifoExecutor::Task task_a, task_b;
    task_a = [&] { ++first; check(scheduler.size(a) == 1,"running work uncharged"); if (refill) scheduler.post(a,task_a); };
    task_b = [&] { ++second; if (refill) scheduler.post(b,task_b); };
    scheduler.post(a,task_a); scheduler.post(b,task_b);
    for (int i = 0; i < 40; ++i) check(scheduler.run_one() == RunResult::executed,"weighted task failed");
    check(first == 30 && second == 10,"weighted owner share wrong");
    refill = false; while (scheduler.run_one() != RunResult::idle) {}
    scheduler.stop(a); scheduler.stop(b); scheduler.remove(a); scheduler.remove(b);
    rejects<ReferenceError>([&] { (void)scheduler.size(a); });
    const auto fresh = scheduler.add_owner(1,1); check(fresh != a,"removed owner identity reused");
}
// 验证未满足前置不占执行机会，任务失败独立，停止和递归推进不抹除工作。
// 步骤：排队等待及就绪任务 → 验证资格与异常 → 结束前置并继续 → 验证停止和容量。
void eligibility() {
    OwnerScheduler scheduler(2); const auto a = scheduler.add_owner(64,2), b = scheduler.add_owner(1,1);
    OperationSource pending; std::vector<int> order;
    scheduler.post(a,[&] { order.push_back(1); },pending.operation());
    scheduler.post(a,[&] { order.push_back(2); });
    scheduler.post(b,[&] { order.push_back(3); throw std::runtime_error("isolated"); });
    rejects<std::length_error>([&] { scheduler.post(a,[] {}); });
    check(scheduler.run_one() == RunResult::executed && order == std::vector<int>{2},"waiting prerequisite blocked ready owner work");
    check(scheduler.run_one() == RunResult::failed && order == std::vector<int>({2,3}),"idle weighted owner blocked independent failure");
    check(scheduler.run_one() == RunResult::idle && scheduler.size(a) == 1,"pending task ran or vanished");
    pending.settle(OperationState::cancelled); scheduler.stop(a);
    rejects<std::logic_error>([&] { scheduler.post(a,[] {}); });
    rejects<std::logic_error>([&] { scheduler.remove(a); });
    check(scheduler.run_one() == RunResult::executed && order.back() == 1,"cancelled prerequisite lost cleanup step");
    scheduler.remove(a);
    scheduler.post(b,[&] { rejects<std::logic_error>([&] { (void)scheduler.run_one(); }); rejects<std::length_error>([&] { scheduler.post(b,[] {}); }); });
    check(scheduler.run_one() == RunResult::executed && !scheduler.size(b),"activity guard or active capacity failed");
}
// 验证优先级份额和独立控制预留，普通过载仍能执行取消收尾。
// 步骤：持续排队全部普通级别 → 核对固定份额和同级顺序 → 填满普通队列 → 验证控制与普通交替。
void priorities() {
    OwnerScheduler scheduler(1); const auto owner = scheduler.add_owner(1,21,6); std::vector<int> trace;
    for (int i = 0; i < 7; ++i) {
        scheduler.post(owner,[&,i] { trace.push_back(100+i); },{},TaskPriority::high);
        scheduler.post(owner,[&,i] { trace.push_back(200+i); },{},TaskPriority::normal);
        scheduler.post(owner,[&,i] { trace.push_back(300+i); },{},TaskPriority::low);
    }
    check(!scheduler.available(owner) && scheduler.available(owner,TaskPriority::control) == 6,"ordinary work consumed control reserve");
    rejects<std::length_error>([&] { scheduler.post(owner,[] {}); });
    for (int i = 0; i < 7; ++i) scheduler.run_one();
    check(trace == std::vector<int>({100,101,102,103,200,201,300}),"priority shares or same-level FIFO wrong");
    trace.clear();
    for (int i = 0; i < 6; ++i) scheduler.post(owner,[&,i] { trace.push_back(i); },{},TaskPriority::control);
    rejects<std::length_error>([&] { scheduler.post(owner,[] {},{},TaskPriority::control); });
    for (int i = 0; i < 5; ++i) scheduler.run_one();
    check(trace == std::vector<int>({0,1,2,3,104}),"control burst starved ordinary work or used wrong reserve");
    while (scheduler.run_one() != RunResult::idle) {}
    rejects<std::invalid_argument>([&] { scheduler.post(owner,[] {},{},static_cast<TaskPriority>(99)); });
    OperationSource pending;
    scheduler.post(owner,[&] { trace.push_back(400); },pending.operation(),TaskPriority::high);
    scheduler.post(owner,[&] { trace.push_back(500); },{},TaskPriority::low);
    scheduler.run_one(); check(trace.back() == 500,"priority bypassed prerequisite eligibility");
    pending.settle(OperationState::succeeded); scheduler.run_one(); check(trace.back() == 400,"ready high priority lost");
}
}
// 验收所有者有界调度、前置资格、权重公平和活动保护。
// 步骤：验证持续竞争 → 验证等待及失败隔离 → 报告结果。
int main() {
    try { fairness(); eligibility(); priorities(); std::cout << "Owner scheduler checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
