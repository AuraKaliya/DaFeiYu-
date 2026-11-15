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
}
// 执行固定工具工作夹具，不调用真实模型。
int main() {
    try { lifecycle(); retention(); std::cout << "Tool work checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
