#include <dafeiyu/retry_controller.hpp>
#include <dafeiyu/owner_scheduler.hpp>
#include <iostream>
namespace {
using namespace dfy::runtime;
using namespace std::chrono_literals;
// 在重试次数、实际退出或退避取消违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 验证明确错误类别，不吞没无关异常。
template<class Error, class Action>
void rejects(Action action) { try { action(); } catch (const Error&) { return; } throw std::runtime_error("retry accepted invalid action"); }
// 验证安全失败在有界退避后重新通过调度，旧尝试不能完成新尝试。
// 步骤：开始并确认首项退出 → 退避期间运行其他工作 → 到期调度重试 → 核对旧身份及唯一结果。
void reschedule() {
    const RetryTime origin{}; RetryController retry({3,100ms,150ms},origin,origin+10s);
    const auto first = retry.begin_attempt(origin); check(first && !retry.begin_attempt(origin),"concurrent retry began");
    retry.finish_attempt(*first,OperationState::failed,true,origin);
    const auto ready = retry.readiness(); OwnerScheduler scheduler(2); const auto owner = scheduler.add_owner(1,1), other = scheduler.add_owner(1,1);
    std::optional<AttemptId> second; unsigned unrelated = 0;
    scheduler.post(owner,[&] { second = retry.begin_attempt(origin+100ms); },ready);
    scheduler.post(other,[&] { ++unrelated; });
    check(scheduler.run_one() == RunResult::executed && unrelated == 1 && !second && retry.attempts() == 1,"backoff blocked unrelated owner or consumed attempt");
    retry.advance(origin+99ms); check(scheduler.run_one() == RunResult::idle,"retry ran before delay");
    retry.advance(origin+100ms); check(!second && scheduler.run_one() == RunResult::executed && second && second != first,"retry readiness ran work inline or reused identity");
    rejects<std::logic_error>([&] { retry.finish_attempt(*first,OperationState::succeeded,false,origin+100ms); });
    retry.finish_attempt(*second,OperationState::failed,true,origin+100ms);
    retry.advance(origin+249ms); check(!retry.begin_attempt(origin+249ms),"backoff cap not respected");
    const auto third = retry.begin_attempt(origin+250ms); check(third && third->number == 3,"third retry not scheduled");
    retry.finish_attempt(*third,OperationState::succeeded,false,origin+250ms);
    check(retry.operation()->state() == OperationState::succeeded && retry.phase() == RetryPhase::finished && retry.attempt(1).exited == origin+100ms,
        "retry final result or attempt history wrong");
    rejects<std::logic_error>([&] { retry.finish_attempt(*third,OperationState::failed,false,origin+250ms); });
}
// 验证默认无重试、明确安全条件、取消退避及运行中期限与实际退出分离。
// 步骤：耗尽默认和显式次数 → 拒绝不安全重放 → 取消等待 → 运行中超时并接受迟到事实。
void boundaries() {
    const RetryTime origin{};
    for (const auto policy : {RetryPolicy{},RetryPolicy{2,1ms,1ms}}) {
        RetryController bounded(policy,origin,origin+1s);
        for (std::size_t i = 0; i < policy.maximum_attempts; ++i) {
            const auto time = origin + std::chrono::milliseconds(i); const auto id = bounded.begin_attempt(time);
            check(id.has_value(),"bounded retry missing attempt"); bounded.finish_attempt(*id,OperationState::failed,true,time);
        }
        check(bounded.operation()->state() == OperationState::failed && !bounded.readiness(),"attempt budget allowed another retry");
    }
    RetryController unsafe({3,1ms,1ms},origin,origin+1s); auto id = unsafe.begin_attempt(origin);
    unsafe.finish_attempt(*id,OperationState::failed,false,origin); check(unsafe.attempts() == 1 && unsafe.phase() == RetryPhase::finished,"unsafe result replayed");
    RetryController cancelled({2,100ms,100ms},origin,origin+1s); id = cancelled.begin_attempt(origin);
    cancelled.finish_attempt(*id,OperationState::failed,true,origin); const auto waiting = cancelled.readiness();
    cancelled.request_cancel(CancelReason::user); cancelled.advance(origin+1ms);
    check(waiting->state() == OperationState::cancelled && !cancelled.begin_attempt(origin+200ms) && cancelled.attempts() == 1,"cancelled backoff retried or did not wake");
    RetryController timeout({2,100ms,100ms},origin,origin+1s); id = timeout.begin_attempt(origin); timeout.advance(origin+1s);
    check(timeout.operation()->state() == OperationState::cancelled && timeout.phase() == RetryPhase::running && !timeout.attempt(0).exited,"timeout forged actual exit");
    timeout.finish_attempt(*id,OperationState::succeeded,false,origin+2s);
    check(timeout.phase() == RetryPhase::finished && timeout.operation()->state() == OperationState::cancelled && timeout.attempt(0).state == OperationState::succeeded,
        "late attempt overwrote deadline or lost its fact");
    rejects<std::invalid_argument>([&] { timeout.advance(origin); });
    rejects<std::invalid_argument>([&] { RetryController invalid({65,1ms,1ms},origin,origin+1s); });
    auto budget = std::make_shared<Budget>(BudgetAmount{2,0}); std::shared_ptr<const Operation> abandoned;
    {
        RetryController held({2,100ms,100ms},origin,origin+1s,budget);
        auto first = held.begin_attempt(origin); held.finish_attempt(*first,OperationState::failed,true,origin); abandoned = held.readiness();
        rejects<std::length_error>([&] { RetryController rejected({},origin,origin+1s,budget); });
    }
    check(abandoned->state() == OperationState::cancelled && !budget->used().items,"abandoned backoff leaked metadata or wakeup");
    const auto near_max = RetryTime::max()-50ms;
    RetryController edge({2,100ms,100ms},near_max,RetryTime::max()); id = edge.begin_attempt(near_max);
    edge.finish_attempt(*id,OperationState::failed,true,near_max); edge.advance(RetryTime::max());
    check(edge.phase() == RetryPhase::finished && edge.operation()->state() == OperationState::cancelled,"backoff time overflow bypassed deadline");
}
}
// 验收显式安全的有界尝试、取消退避、唯一结果及实际退出边界。
// 步骤：验证重新调度 → 验证预算、取消及期限 → 报告结果。
int main() {
    try { reschedule(); boundaries(); std::cout << "Retry controller checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
