#include <dafeiyu/retry_controller.hpp>
#include <dafeiyu/processing_queue.hpp>
#include <dafeiyu/owner_scheduler.hpp>
#include <iostream>
namespace {
using namespace dfy::runtime;
using namespace dfy::events;
using namespace std::chrono_literals;
// 在重试期间丢失事件、顺序占用或退出边界时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
}
// 验证同一事件处理跨重试保持身份和顺序，退避只让出线程，期限不伪造实际退出。
// 步骤：接纳两份顺序处理 → 首次失败后退避 → 独立域继续工作 → 重试期间超时 → 实际退出后放行后一处理。
int main() {
    try {
        auto store = std::make_shared<EventStore>(8,0); SubscriptionQueue incoming(store,1);
        const auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.retry",1,{}, {0,32}));
        const auto input = store->accept(std::make_shared<const StructValue>(StructValue::create(type,{},0)));
        incoming.push(input); store->release(input); auto data = incoming.poll().event;
        ProcessingQueue processing(2); const auto first = processing.submit(data), next = processing.submit(data);
        const auto event_id = first->event().id; processing.advance(); processing.start(first);
        const RetryTime origin{}; RetryController retry({2,100ms,100ms},origin,origin+200ms);
        const auto attempt = retry.begin_attempt(origin); retry.finish_attempt(*attempt,OperationState::failed,true,origin);
        processing.advance();
        check(processing.start(next) == StartResult::waiting && first->ordering()->phase() == SerialPhase::running &&
            first->event().id == event_id && retry.phase() == RetryPhase::backoff,"backoff released order or republished event");
        OwnerScheduler scheduler(2); const auto owner = scheduler.add_owner(1,1), other = scheduler.add_owner(1,1);
        std::optional<AttemptId> resumed; bool unrelated = false;
        scheduler.post(owner,[&] { resumed = retry.begin_attempt(origin+100ms); },retry.readiness());
        scheduler.post(other,[&] { unrelated = true; }); scheduler.run_one();
        check(unrelated && !resumed && retry.attempts() == 1,"retry waiting blocked independent processing");
        retry.advance(origin+100ms); check(scheduler.run_one() == RunResult::executed && resumed,"retry did not reenter scheduler");
        retry.advance(origin+200ms); processing.complete(first,retry.operation()->state()); processing.advance();
        check(first->completion()->state() == OperationState::cancelled && first->exit()->state() == OperationState::pending &&
            processing.start(next) == StartResult::waiting,"logical retry deadline released actual processing");
        retry.finish_attempt(*resumed,OperationState::succeeded,false,origin+300ms);
        check(retry.phase() == RetryPhase::finished && retry.attempt(1).state == OperationState::succeeded &&
            first->event().id == event_id,"late attempt changed event or disappeared");
        processing.finish(first); processing.advance();
        check(processing.start(next) == StartResult::started && next->ordering()->sequence() == 2,"actual retry exit did not release original order");
        processing.complete(next,OperationState::succeeded); processing.finish(next); processing.close(CloseMode::drain);
        check(processing.closed() && first->completion()->state() == OperationState::cancelled,"late success overwrote original business result");
        std::cout << "Retry processing checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
