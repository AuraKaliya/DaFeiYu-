#include <dafeiyu/result_group.hpp>
#include <iostream>
namespace {
using namespace dfy::runtime;
// 在汇总、迟到结果或预算契约违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 验证明确异常类别，保留无关失败供入口报告。
template<class Error, class Action>
void rejects(Action action) { try { action(); } catch (const Error&) { return; } throw std::runtime_error("result group accepted invalid report"); }
// 验证四种完成策略与剩余工作策略，迟到参与者不能改写正式结果。
// 步骤：检查唯一和全部成功 → 检查短路失败与取消 → 检查任一成功 → 检查首个报告终态。
void strategies() {
    ResultGroup single(ResultStrategy::single,1,0); single.report(0,OperationState::succeeded);
    check(single.result()->operation()->state() == OperationState::succeeded && single.result()->winner() == 0,"single result wrong");
    rejects<std::logic_error>([&] { single.report(0,OperationState::failed); });
    ResultGroup all(ResultStrategy::all_succeeded,2,0); all.report(1,OperationState::succeeded);
    check(all.result()->operation()->state() == OperationState::pending,"all succeeded settled early");
    all.report(0,OperationState::succeeded); check(!all.result()->winner(),"collective success invented winner");
    ResultGroup failed(ResultStrategy::all_succeeded,2,0,RemainingPolicy::request_cancel); failed.report(1,OperationState::failed);
    check(failed.result()->cancellation_requested(0),"short circuit did not request remaining cancellation");
    failed.report(0,OperationState::succeeded);
    check(failed.result()->operation()->state() == OperationState::failed && failed.result()->winner() == 1 && failed.result()->reported() == 2,"late success overwrote failure");
    ResultGroup any(ResultStrategy::any_succeeded,3,0); any.report(0,OperationState::failed); any.report(1,OperationState::succeeded);
    check(any.result()->winner() == 1 && !any.result()->cancellation_requested(2),"keep-running strategy cancelled remaining participant");
    any.report(2,OperationState::cancelled); check(any.result()->operation()->state() == OperationState::succeeded,"late cancel overwrote success");
    for (const auto last : {OperationState::failed,OperationState::cancelled}) {
        ResultGroup none(ResultStrategy::any_succeeded,2,0); none.report(0,OperationState::cancelled); none.report(1,last);
        check(none.result()->operation()->state() == last && !none.result()->winner(),"any-success exhaustion wrong");
    }
    ResultGroup first(ResultStrategy::first_terminal,2,0); first.report(1,OperationState::cancelled); first.report(0,OperationState::succeeded);
    check(first.result()->winner() == 1 && first.result()->operation()->state() == OperationState::cancelled,"first terminal used participant index order");
}
// 验证取消提交边界和结果数据的完整预算寿命。
// 步骤：主动取消并接受迟到事实 → 拒绝非法报告 → 保留结果值越过控制器退出 → 最后归还预留。
void boundaries() {
    ResultGroup cancelled(ResultStrategy::all_succeeded,2,0);
    check(cancelled.cancel(CancelReason::deadline) == CancelDisposition::recorded && cancelled.result()->cancellation_requested(0),"group cancellation missing");
    cancelled.report(0,OperationState::succeeded);
    check(cancelled.cancel(CancelReason::user) == CancelDisposition::already_settled && cancelled.result()->operation()->cancellation_reason() == CancelReason::deadline,
        "late cancel changed first reason");
    rejects<std::invalid_argument>([&] { cancelled.report(1,OperationState::pending); });
    auto budget = std::make_shared<Budget>(BudgetAmount{2,4}); std::shared_ptr<const Value> retained;
    {
        ResultGroup group(ResultStrategy::single,1,4,RemainingPolicy::keep_running,budget);
        rejects<std::length_error>([&] { group.report(0,OperationState::succeeded,std::make_shared<const Value>(Value::from_uint64(1))); });
        check(!group.result()->reported(),"rejected result committed participant state");
        group.report(0,OperationState::succeeded,std::make_shared<const Value>(Value::from_string("test",4)));
        retained = group.result()->value(0);
    }
    check(budget->used().items == 1 && budget->used().bytes == 4 && retained->as_string() == "test","result value escaped budget or lifetime");
    retained.reset(); check(!budget->used().items && !budget->used().bytes,"result reservation leaked");
    bool charged_during_release = false;
    {
        ResultGroup group(ResultStrategy::single,1,4,RemainingPolicy::keep_running,budget);
        auto value = std::shared_ptr<const Value>(new Value(Value::from_string("held",4)),[&](const Value* item) {
            charged_during_release = budget->used().items == 1 && budget->used().bytes == 4; delete item;
        });
        group.report(0,OperationState::succeeded,std::move(value));
    }
    check(charged_during_release && !budget->used().items,"result reservation returned before value release");
    rejects<std::invalid_argument>([] { ResultGroup invalid(ResultStrategy::single,2,0); });
    rejects<std::invalid_argument>([] { ResultGroup invalid(ResultStrategy::all_succeeded,0,0); });
}
}
// 验收显式业务参与者的唯一结果、汇总策略、迟到事实和有界结果持有。
// 步骤：验证策略 → 验证取消与预算 → 报告结果。
int main() {
    try { strategies(); boundaries(); std::cout << "Result group checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
