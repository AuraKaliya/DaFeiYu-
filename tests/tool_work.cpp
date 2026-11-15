#include <dafeiyu/tool_work.hpp>
#include <iostream>
#include <stdexcept>

namespace {
using namespace dfy::agent;
using namespace dfy::runtime;
// 报告工具生命周期断言失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Gate { bool exit = false, report = false, throw_poll = false, oversize = false; unsigned destroyed = 0; CancelReason cancel = CancelReason::none; };
class Work final : public ToolWork {
  public:
    // 保留测试控制状态，不执行任何外部动作。
    explicit Work(std::shared_ptr<Gate> gate) : gate_(std::move(gate)) {}
    // 标记真实工作对象已销毁，供退出顺序检查。
    ~Work() override { ++gate_->destroyed; }
    // 按控制状态报告结果、取消与退出，模拟结果先于资源回收。
    ToolProgress poll(CancelReason cancellation) override {
        gate_->cancel = cancellation;
        if (gate_->throw_poll) throw std::runtime_error("controlled poll failure");
        return {gate_->report ? std::optional<ToolResult>({true,ToolEffect::applied,
            gate_->oversize ? std::string(64*1024+1,'x') : "done",ToolError::none}) : std::nullopt,gate_->exit};
    }
  private:
    std::shared_ptr<Gate> gate_;
};
// 创建合法的固定运行及调用身份，不产生模型请求。
ToolInvocation invocation() { return {AttemptId{StepId{RunId{1,1},1},1},1,{"call","fixture","{}"}}; }
// 验证接纳、取消、迟到效果与实际退出分别保留。
// 步骤：开始前取消 → 业务先完成但保留工作 → 取消后记录迟到效果。
void lifecycle() {
    unsigned starts = 0; auto gate = std::make_shared<Gate>();
    const ToolFactory factory = [&](const auto&) { ++starts; return std::make_unique<Work>(gate); };
    { ToolDriver driver(invocation(),factory); const auto request = driver.request(); request->request_cancel(CancelReason::user); driver.advance();
      check(!starts && request->operation()->state() == OperationState::cancelled && request->exit()->state() == OperationState::succeeded &&
          request->outcome().effect == ToolEffect::not_applied,"cancel before start invoked a tool"); }
    {
        ToolDriver driver(invocation(),factory); const auto request = driver.request(); gate->report = true; driver.advance();
        check(starts == 1 && request->operation()->state() == OperationState::succeeded &&
            request->exit()->state() == OperationState::pending && !gate->destroyed,"business completion released active work");
        gate->exit = true; driver.advance(); check(gate->destroyed == 1 && request->outcome().effect == ToolEffect::applied,"exit lost effect");
    }
    gate = std::make_shared<Gate>();
    {
        ToolDriver driver(invocation(),factory); const auto request = driver.request(); driver.advance();
        request->request_cancel(CancelReason::user); driver.advance();
        check(request->operation()->state() == OperationState::cancelled && request->exit()->state() == OperationState::pending,"cancel forged exit");
        gate->report = gate->exit = true; driver.advance();
        check(request->operation()->state() == OperationState::cancelled && request->outcome().effect == ToolEffect::applied &&
            gate->cancel == CancelReason::user,"late effect was erased by cancellation");
    }
    gate = std::make_shared<Gate>(); gate->report = gate->exit = gate->oversize = true;
    { ToolDriver driver(invocation(),factory); driver.advance(); const auto request = driver.request();
      check(request->operation()->state() == OperationState::failed && request->outcome().effect == ToolEffect::applied &&
          request->outcome().error == ToolError::invalid_result && request->exit()->state() == OperationState::succeeded,
          "invalid output erased known effects or lost actual exit"); }
}
// 验证推进异常不释放在途代码，失去局部驱动后仍可重试真实收尾。
void retention() {
    auto gate = std::make_shared<Gate>(); std::shared_ptr<ToolRequest> request;
    { ToolDriver driver(invocation(),[&](const auto&) { return std::make_unique<Work>(gate); }); request = driver.request();
      driver.advance(); gate->throw_poll = true; driver.advance(); }
    check(ToolDriver::retained() == 1 && !gate->destroyed && request->exit()->state() == OperationState::pending,
        "failed poll or owner destruction released active work");
    gate->throw_poll = false; gate->report = gate->exit = true;
    check(!ToolDriver::retry_retained() && gate->destroyed == 1 && request->exit()->state() == OperationState::succeeded,
        "retained work did not finish");
}
// 验证原生调用返回前发出的取消仍优先结算，实际效果可以随后成功报告。
void callback_cancel() {
    std::shared_ptr<ToolRequest> request;
    ToolDriver driver(invocation(),[&](const auto&) {
        request->request_cancel(CancelReason::user);
        return completed_tool({true,ToolEffect::applied,"done",ToolError::none});
    });
    request = driver.request(); driver.advance();
    check(request->operation()->state() == OperationState::cancelled && request->outcome().effect == ToolEffect::applied &&
        request->exit()->state() == OperationState::succeeded,"callback cancellation lost to returned success");
}
// 验证真实顺序域只在实际退出后让出占用，保留结果继续占据原分层预算。
// 步骤：接纳两项顺序工作 → 提前业务完成但阻塞后项 → 实际退出后放行 → 释放结果归还预算。
void resources() {
    auto parent = std::make_shared<Budget>(BudgetAmount{2,256});
    auto budget = std::make_shared<Budget>(BudgetAmount{2,256},parent);
    auto domain = std::make_shared<SerialDomain>(2); auto gate = std::make_shared<Gate>();
    const auto first = domain->submit(), second = domain->submit(); unsigned starts = 0;
    auto a = std::make_unique<ToolDriver>(invocation(),[&](const auto&) { ++starts; return std::make_unique<Work>(gate); },
        ToolResources{domain,first,budget->acquire({1,128})});
    auto b = std::make_unique<ToolDriver>(invocation(),[&](const auto&) { ++starts; return completed_tool({true,ToolEffect::not_applied,"ok",ToolError::none}); },
        ToolResources{domain,second,budget->acquire({1,128})});
    auto retained = a->request(); gate->report = true;
    a->advance(); check(!starts,"driver granted its own ordering");
    domain->advance(); a->advance(); b->advance();
    check(starts == 1 && retained->operation()->state() == OperationState::succeeded &&
        first->phase() == SerialPhase::running && second->phase() == SerialPhase::waiting,"business completion released ordering");
    gate->exit = true; a->advance(); domain->advance(); b->advance();
    check(starts == 2 && second->phase() == SerialPhase::released && parent->used().items == 2,"exit lost ordering or retained budget");
    a.reset(); b.reset(); check(parent->used().items == 1,"retained result released its budget early");
    retained.reset(); check(parent->used().items == 0 && budget->used().bytes == 0,"result budget leaked");
}
// 验证保留工作仍阻止共享顺序域前进，域关闭会取消未开始工作而不调用工厂。
void ordering_retention() {
    auto domain = std::make_shared<SerialDomain>(2); auto gate = std::make_shared<Gate>();
    const auto first = domain->submit();
    { ToolDriver a(invocation(),[&](const auto&) { return std::make_unique<Work>(gate); },{domain,first,{}});
      domain->advance(); a.advance(); }
    const auto second = domain->submit(); unsigned starts = 0;
    ToolDriver b(invocation(),[&](const auto&) { ++starts; return completed_tool({true,ToolEffect::not_applied,"ok",ToolError::none}); },{domain,second,{}});
    domain->advance(); b.advance(); check(!starts && first->phase() == SerialPhase::running,"orphan work lost ordering");
    domain->close(SerialClose::cancel); b.advance();
    check(!starts && b.request()->operation()->state() == OperationState::cancelled && b.request()->exit()->state() == OperationState::succeeded,
        "closed domain started pending work");
    gate->report = gate->exit = true; check(!ToolDriver::retry_retained() && domain->closed(),"orphan exit retained serial domain");
}
}
// 执行固定工具工作夹具，不调用真实模型。
int main() {
    try { lifecycle(); retention(); callback_cancel(); resources(); ordering_retention(); std::cout << "Tool work checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
