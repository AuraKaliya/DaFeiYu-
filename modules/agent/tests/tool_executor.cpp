#include <dafeiyu/tool_executor.hpp>
#include <iostream>
#include <stdexcept>

namespace {
using namespace dfy::agent;
using namespace dfy::runtime;
// 报告运行级调用约束失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 确认错误命令在实际动作之前被拒绝。
void rejects(const auto& action) {
    try { action(); } catch (const std::logic_error&) { return; }
    throw std::runtime_error("invalid tool execution accepted");
}
// 创建无参数的固定状态修改描述。
ToolDescription description(bool idempotent = false) {
    return {"fixture/action",9,1,{"action","fixture",R"({"type":"object","properties":{},"additionalProperties":false})"},true,idempotent};
}
// 验证同命令复用回执、冲突零调用、有限串行执行和关闭后的结果查询。
// 步骤：接纳并重放同命令 → 实际执行一次 → 验证不同命令及容量 → 关闭仍复用旧回执。
void receipts() {
    ToolCatalog catalog; unsigned calls = 0;
    catalog.add(description(),[&](const auto&) { ++calls; return completed_tool({true,ToolEffect::applied,"ok",ToolError::none}); });
    const std::vector<std::string> ids{"fixture/action"}; const AttemptId attempt{StepId{RunId{1,1},1},1};
    auto parent = std::make_shared<Budget>(BudgetAmount{2,1024*1024}); std::shared_ptr<ToolRequest> retained;
    {
        ToolExecutor executor({1,1},catalog.snapshot(ids,true),{2},parent);
        const auto first = executor.submit(attempt,{"one","action","{}"}); retained = first;
        check(executor.submit(attempt,{"one","action","{ }"}) == first && !calls,"duplicate command started another invocation");
        rejects([&] { executor.submit(attempt,{"two","action","{}"}); });
        rejects([&] { executor.submit(attempt,{"one","action",R"({"changed":1})"}); });
        executor.advance(); check(calls == 1 && !executor.busy(),"first action did not exit");
        auto next = attempt; next.step.sequence = 2;
        executor.submit(next,{"one","action","{}"}); executor.advance();
        check(calls == 2 && executor.size() == 2,"known applied action with new ID was not explicit");
        rejects([&] { executor.submit(attempt,{"three","action","{}"}); });
        executor.close(); check(executor.submit(attempt,{"one","action","{}"}) == first,"closed execution lost existing receipt");
    }
    check(parent->used().items == 1,"retained result lost parent budget"); retained.reset();
    check(parent->used().items == 0,"run result budget leaked");
}
// 验证失败动作已发生或未知时不能盲目重试，确定无效果或声明幂等时允许显式接纳。
// 步骤：报告三种效果 → 在新步骤重复局部调用 ID → 核对幂等和非幂等策略。
void repeat_effects() {
    const std::vector<std::string> ids{"fixture/action"}; const AttemptId attempt{StepId{RunId{1,1},1},1};
    for (const bool idempotent : {false,true}) for (const auto effect : {ToolEffect::unknown,ToolEffect::not_applied,ToolEffect::applied}) {
        ToolCatalog catalog; unsigned calls = 0;
        catalog.add(description(idempotent),[&](const auto&) { ++calls; return completed_tool({false,effect,{},ToolError::failed}); });
        ToolExecutor executor({1,1},catalog.snapshot(ids,true)); executor.submit(attempt,{"one","action","{}"}); executor.advance();
        const bool blocked = effect != ToolEffect::not_applied && !idempotent;
        auto next = attempt; next.step.sequence = 2;
        if (blocked) rejects([&] { executor.submit(next,{"one","action","{}"}); });
        else { executor.submit(next,{"one","action","{}"}); executor.advance(); }
        check(calls == (blocked ? 1u : 2u),"repeat effect policy changed actual invocation count");
    }
}
// 验证接纳期限和父级预算拒绝均不执行工具。
void limits() {
    ToolCatalog catalog; unsigned calls = 0;
    catalog.add(description(true),[&](const auto&) { ++calls; return completed_tool({true,ToolEffect::applied,"ok",ToolError::none}); });
    const std::vector<std::string> ids{"fixture/action"}; const AttemptId attempt{StepId{RunId{1,1},1},1};
    auto clock = std::make_shared<ToolTime>(ToolTime{});
    ToolExecutor executor({1,1},catalog.snapshot(ids,true),{2,std::chrono::milliseconds(1)},{},[clock] { return *clock; });
    auto first = executor.submit(attempt,{"one","action","{}"}); *clock += std::chrono::milliseconds(1); executor.advance();
    check(!calls && !first->dispatched() && first->exit()->cancellation_reason() == CancelReason::deadline,"expired work executed");
    auto parent = std::make_shared<Budget>(BudgetAmount{1,1});
    ToolExecutor denied({1,1},catalog.snapshot(ids,true),{},parent);
    rejects([&] { denied.submit(attempt,{"one","action","{}"}); });
    check(!denied.size() && !parent->used().items && !calls,"budget refusal left admitted work");
}
// 验证只有变更工具需要最终行动检查，失败回执重试不因检查恢复而重做动作。
// 步骤：接纳只读与变更候选 → 在实际开始前拒绝变更 → 复用原失败回执 → 显式新命令重新检查。
void action_guard() {
    ToolCatalog catalog; unsigned reads = 0, changes = 0, checks = 0; bool allowed = false;
    auto read = description(); read.id = "fixture/read"; read.model.name = "read"; read.changes_state = false;
    catalog.add(read,[&](const auto&) { ++reads; return completed_tool({true,ToolEffect::not_applied,"",ToolError::none}); });
    catalog.add(description(),[&](const auto&) { ++changes; return completed_tool({true,ToolEffect::applied,"",ToolError::none}); });
    const std::vector<std::string> ids{"fixture/read","fixture/action"};
    ToolExecutor executor({1,1},catalog.snapshot(ids,true),{}, {},std::chrono::steady_clock::now,[&] { ++checks; return allowed; });
    const AttemptId attempt{{{1,1},1},1}; const auto reading = executor.submit(attempt,{"read","read","{}"});
    check(!reading->dispatched(),"queued read counted as dispatched");
    const auto denied = executor.submit(attempt,{"change","action","{}"}); executor.advance(); executor.advance();
    check(reads == 1 && !changes && checks == 1 && reading->dispatched() && !denied->dispatched() &&
        denied->outcome().effect == ToolEffect::not_applied && denied->operation()->state() == OperationState::failed,
        "action guard affected read or admitted change");
    allowed = true; check(executor.submit(attempt,{"change","action","{}"}) == denied,"guard recovery replaced original receipt");
    executor.advance(); check(!changes && checks == 1,"old command repeated after guard recovered");
    executor.submit({{{1,1},2},1},{"change","action","{}"}); executor.advance();
    check(changes == 1 && checks == 2,"new explicit command did not recheck action precondition");
}
}
// 验证固定工具和真实预算，不调用模型。
int main() {
    try { receipts(); repeat_effects(); limits(); action_guard(); std::cout << "Tool executor checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
