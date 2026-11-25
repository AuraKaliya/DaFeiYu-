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
// 验证截止前后的取消边界，超时不调用未开始工具，也不抹掉回调期间发生的效果。
// 步骤：到期前接纳但延迟推进 → 超时拒绝首次调用 → 模拟调用跨越截止时刻并保留效果。
void deadlines() {
    auto clock = std::make_shared<ToolTime>(ToolTime{}); unsigned calls = 0;
    const auto deadline = *clock+std::chrono::seconds(1);
    ToolResources resources; resources.deadline = deadline; resources.now = [clock] { return *clock; };
    ToolDriver first(invocation(),[&](const auto&) { ++calls; return completed_tool({true,ToolEffect::applied,"ok",ToolError::none}); },std::move(resources));
    *clock = deadline; first.advance();
    check(!calls && first.request()->operation()->state() == OperationState::cancelled &&
        first.request()->exit()->cancellation_reason() == CancelReason::deadline &&
        first.request()->outcome().effect == ToolEffect::not_applied,"expired pending call executed");
    *clock = ToolTime{}; ToolResources during; during.deadline = deadline; during.now = [clock] { return *clock; };
    ToolDriver second(invocation(),[&](const auto&) {
        ++calls; *clock = deadline;
        return completed_tool({true,ToolEffect::applied,"changed",ToolError::none});
    },std::move(during));
    second.advance();
    check(calls == 1 && second.request()->operation()->state() == OperationState::cancelled &&
        second.request()->outcome().effect == ToolEffect::applied && second.request()->exit()->state() == OperationState::succeeded,
        "callback crossing deadline forged success or erased effect");
}
// 验证业务完成后的清理故障仍可查询，损坏文本不会丢弃已知动作效果。
// 步骤：报告成功后制造清理故障 → 保留原结果并收尾 → 拒绝非法 UTF-8 输出。
void faults() {
    auto gate = std::make_shared<Gate>(); gate->report = true;
    ToolDriver driver(invocation(),[gate](const auto&) { return std::make_unique<Work>(gate); });
    driver.advance(); gate->throw_poll = true; driver.advance();
    check(driver.request()->operation()->state() == OperationState::succeeded && driver.request()->fault() == ToolError::invalid_result &&
        driver.request()->exit()->state() == OperationState::pending,"cleanup failure was hidden by prior business success");
    gate->throw_poll = false; gate->exit = true; driver.advance();
    ToolDriver invalid(invocation(),[](const auto&) { return completed_tool({true,ToolEffect::applied,std::string(1,static_cast<char>(0xff)),ToolError::none}); });
    invalid.advance();
    check(invalid.request()->operation()->state() == OperationState::failed && invalid.request()->outcome().effect == ToolEffect::applied &&
        invalid.request()->fault() == ToolError::invalid_result && invalid.request()->outcome().output.empty(),"invalid UTF-8 escaped or erased effect");
}
// 验证最终前置检查发生在工厂之前，检查期间取消或到期仍优先停止。
// 步骤：注入拒绝、异常或停止 → 推进获准顺序 → 核对零动作与明确退出。
void preconditions() {
    for (unsigned mode = 0; mode < 5; ++mode) {
        auto domain = std::make_shared<SerialDomain>(1); const auto ordering = domain->submit();
        ToolTime now{}; unsigned starts = 0, checks = 0; std::shared_ptr<ToolRequest> request;
        ToolResources resources{domain,ordering,{},now+std::chrono::milliseconds(1),[&] { return now; }};
        resources.before_start = [&] {
            ++checks;
            if (mode == 0) return false;
            if (mode == 1) throw std::runtime_error("precondition unavailable");
            if (mode == 2 || mode == 4) request->request_cancel(CancelReason::user);
            if (mode == 3) now += std::chrono::milliseconds(1);
            return mode != 4;
        };
        ToolDriver driver(invocation(),[&](const auto&) { ++starts; return completed_tool({true,ToolEffect::applied,"",ToolError::none}); },std::move(resources));
        request = driver.request(); driver.advance(); check(!checks,"precondition ran before ordering grant");
        domain->advance(); driver.advance();
        check(checks == 1 && !starts && request->outcome().effect == ToolEffect::not_applied && request->exit()->state() == OperationState::succeeded &&
            request->operation()->state() == (mode < 2 ? OperationState::failed : OperationState::cancelled),"precondition started work or forged unknown effect");
    }
    auto gate = std::make_shared<Gate>(); gate->report = true; unsigned checks = 0;
    ToolResources resources; resources.before_start = [&] { return ++checks == 1; };
    ToolDriver driver(invocation(),[gate](const auto&) { return std::make_unique<Work>(gate); },std::move(resources));
    driver.advance(); gate->exit = true; driver.advance();
    check(checks == 1 && driver.request()->outcome().effect == ToolEffect::applied && driver.request()->exit()->state() == OperationState::succeeded,
        "precondition reran during cleanup or erased applied effect");
}
}
// 执行固定工具工作夹具，不调用真实模型。
int main() {
    try { lifecycle(); retention(); callback_cancel(); resources(); ordering_retention(); deadlines(); faults(); preconditions(); std::cout << "Tool work checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
