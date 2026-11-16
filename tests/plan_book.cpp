#include <dafeiyu/plan_book.hpp>
#include <iostream>

namespace {
using namespace dfy::agent;
using namespace dfy::runtime;
// 报告计划顺序、版本或实际工作关联错误。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 验证进行中、旧版本或无证据推进被拒绝。
void rejects(const auto& action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid plan transition accepted");
}
// 创建有两个声明式步骤的候选，不包含执行状态。
std::vector<PlanStepDefinition> steps() { return {{"read","读取状态","宿主核对回执",{}},{"summary","汇总事实","宿主核对回答",{}}}; }
struct Gate { bool release = false; unsigned calls = 0; };
class Work final : public ToolWork {
  public:
    // 保留测试控制的实际退出边界。
    explicit Work(std::shared_ptr<Gate> gate) : gate_(std::move(gate)) { ++gate_->calls; }
    // 成功结果可早于实际退出，不由计划查询推进释放。
    ToolProgress poll(CancelReason) override { return {ToolResult{true,ToolEffect::not_applied,"value",ToolError::none},gate_->release}; }
  private:
    std::shared_ptr<Gate> gate_;
};
// 验证原工作成功、退出和宿主确认分开，修订只改变尚未开始的尾部。
// 步骤：关联未启动工作 → 验证进行中及退出等待 → 明确验证完成 → 修订尾部并保留旧证据。
void sequential() {
    PlanBook book(PlanScope({1,1},1)); book.install(book.binding(),steps()); const PlanStepKey first{book.binding(),"read"};
    auto gate = std::make_shared<Gate>(); ToolDriver driver({{{{1,1},1},1},1,{"call","read","{}"}},[gate](const auto&) { return std::make_unique<Work>(gate); });
    const PlanWork work{driver.request()->invocation().attempt,driver.request()->operation(),driver.request()->exit()};
    rejects([&] { book.start({book.binding(),"summary"},work); }); book.start(first,work);
    check(!gate->calls && book.snapshot().steps[0].waiting == PlanWait::work,"plan started work or forged completion");
    rejects([&] { book.confirm(first,"checked"); }); driver.advance();
    check(book.snapshot().steps[0].waiting == PlanWait::exit && gate->calls == 1,"result hid actual exit wait");
    rejects([&] { book.install(book.binding(),steps()); }); rejects([&] { book.confirm(first,"checked"); });
    gate->release = true; driver.advance(); check(book.snapshot().steps[0].waiting == PlanWait::verification,"work success auto-verified plan");
    book.confirm(first,"checked"); book.confirm(first,"checked"); rejects([&] { book.confirm(first,"changed"); });
    auto next = steps(); next[1].objective = "重新整理事实"; const auto old = book.snapshot(); book.install(book.binding(),next);
    check(book.binding().plan_revision == 2 && book.snapshot().steps[0].key == first && old.definition->steps()[1].objective == "汇总事实",
        "revision rewrote executed prefix or old snapshot");
    book.confirm(first,"checked"); auto changed = next; changed[0].objective = "改写历史"; rejects([&] { book.install(book.binding(),changed); });
    rejects([&] { book.start({book.binding(),"summary"},work); });
    book.skip({book.binding(),"summary"},"host_not_needed"); check(book.snapshot().steps[1].state == PlanStepState::skipped && gate->calls == 1,"skip executed work");
}
// 验证失败工作和停止不能被新计划或模型式完成声明擦除。
// 步骤：拒绝错绑 → 接收原失败 → 拒绝确认及改写 → 保留停止后的原效果。
void failure_and_stop() {
    PlanBook book(PlanScope({2,1},1)); book.install(book.binding(),steps()); const PlanStepKey key{book.binding(),"read"};
    ToolDriver driver({{{{2,1},1},1},2,{"call","read","{}"}},[](const auto&) { return completed_tool({false,ToolEffect::unknown,"",ToolError::failed}); });
    auto work = PlanWork{driver.request()->invocation().attempt,driver.request()->operation(),driver.request()->exit()};
    auto invalid = work; invalid.execution.step.run = {3,1}; rejects([&] { book.start(key,invalid); });
    invalid = work; invalid.exit = invalid.result; rejects([&] { book.start(key,invalid); });
    book.start(key,work); driver.advance(); check(book.snapshot().steps[0].state == PlanStepState::failed,"original failure missing");
    rejects([&] { book.confirm(key,"model_says_done"); }); rejects([&] { book.install(book.binding(),steps()); });
    rejects([&] { book.skip(key,"ignore_failure"); }); book.stop();
    check(book.snapshot().stopped && book.snapshot().steps[1].waiting == PlanWait::stopped && driver.request()->outcome().effect == ToolEffect::unknown,
        "stop erased original failure or effect");
}
// 验证停止不会伪造实际退出，取消与成功等待宿主验证仍分别显示。
// 步骤：停止已完成结果但未退出的工作 → 确认原退出 → 验证停止后不能推进 → 保留原取消状态。
void stopped_work() {
    PlanBook book(PlanScope({3,1},1)); book.install(book.binding(),steps()); const PlanStepKey key{book.binding(),"read"};
    auto gate = std::make_shared<Gate>(); ToolDriver driver({{{{3,1},1},1},3,{"call","read","{}"}},[gate](const auto&) { return std::make_unique<Work>(gate); });
    book.start(key,{driver.request()->invocation().attempt,driver.request()->operation(),driver.request()->exit()}); driver.advance(); book.stop();
    check(book.snapshot().steps[0].waiting == PlanWait::exit && driver.request()->exit()->state() == OperationState::pending,"plan stop forged exit");
    gate->release = true; driver.advance(); rejects([&] { book.confirm(key,"checked"); });
    check(book.snapshot().steps[0].waiting == PlanWait::stopped && book.snapshot().steps[0].state == PlanStepState::running,"stopped work falsely requested verification");
    PlanBook cancelled(PlanScope({4,1},1)); cancelled.install(cancelled.binding(),steps());
    unsigned calls = 0;
    ToolDriver skipped({{{{4,1},1},1},4,{"call","read","{}"}},[&](const auto&) { ++calls; return completed_tool({true,ToolEffect::not_applied,"",ToolError::none}); });
    cancelled.start({cancelled.binding(),"read"},{skipped.request()->invocation().attempt,skipped.request()->operation(),skipped.request()->exit()});
    skipped.request()->request_cancel(CancelReason::user); skipped.advance();
    check(cancelled.snapshot().steps[0].state == PlanStepState::cancelled && !calls,"original cancellation became plan failure or ran work");
}
}
// 使用真实工具操作与可控退出验证计划记录，不调用模型。
int main() {
    try { sequential(); failure_and_stop(); stopped_work(); std::cout << "Plan book checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
