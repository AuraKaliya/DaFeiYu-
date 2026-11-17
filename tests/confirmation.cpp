#include <dafeiyu/confirmation.hpp>
#include <iostream>

namespace {
using namespace dfy::agent;
using namespace dfy::runtime;
// 报告确认与有界控制契约不一致。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 验证错误命令明确拒绝且不被误认为测试通过。
template<class F> void rejects(F action) {
    bool rejected = false; try { action(); } catch (const std::exception&) { rejected = true; }
    check(rejected,"invalid confirmation accepted");
}
// 创建有完整版本和规范参数的宿主候选值。
ConfirmationTarget target() {
    return {{{{1,1},1},1},1,1,4,5,1,"fixture/change","call","change","{}",ToolTime{}+std::chrono::seconds(1)};
}
// 验证身份、版本和参数绑定，重复命令复用而批准只消费一次。
void decisions() {
    ConfirmationCaller caller, stranger; ConfirmationBook book({1,1},caller);
    const auto id = book.offer(target()); auto view = book.snapshot().front();
    ConfirmationCommand command{10,id,view.revision,view.target,true};
    rejects([&] { book.decide(stranger,command,{}); });
    auto changed = command; changed.expected.attempt.step.run.sequence = 2;
    rejects([&] { book.decide(caller,changed,{}); });
    changed = command; changed.expected.policy_revision = 2; rejects([&] { book.decide(caller,changed,{}); });
    changed = command; changed.expected.arguments = R"({"x":1})"; rejects([&] { book.decide(caller,changed,{}); });
    changed = command; changed.expected_revision = 2; rejects([&] { book.decide(caller,changed,{}); });
    const auto receipt = book.decide(caller,command,{});
    check(receipt == book.decide(caller,command,{}),"duplicate decision changed receipt");
    changed = command; changed.approve = false; rejects([&] { book.decide(caller,changed,{}); });
    check(book.consume(id,target(),{}) && !book.consume(id,target(),{}),"approval executed twice");
    check(book.decide(caller,command,ToolTime{}+std::chrono::seconds(2)) == receipt && book.state(id) == ConfirmationState::consumed,
        "replayed receipt restored consumed permission");
}
// 验证拒绝防重复询问、运行容量、取消和期限不能转为批准。
void stops() {
    ConfirmationCaller caller; ConfirmationBook book({1,1},caller,2); const auto first = book.offer(target());
    book.decide(caller,{1,first,1,target(),false},{});
    auto another = target(); another.call_id = "new"; another.policy_revision = 2;
    rejects([&] { book.offer(another); });
    another.arguments = R"({"x":1})"; const auto second = book.offer(another);
    rejects([&] { book.offer(another); });
    book.advance(another.deadline);
    check(book.state(first) == ConfirmationState::denied && book.state(second) == ConfirmationState::expired,"deadline erased denial or allowed work");
    rejects([&] { book.decide(caller,{2,second,1,another,true},another.deadline); });
    ConfirmationBook cancelled({1,1},caller); const auto id = cancelled.offer(target());
    cancelled.decide(caller,{3,id,1,target(),true},{}); cancelled.advance({},CancelReason::user);
    check(!cancelled.consume(id,target(),{}) && cancelled.state(id) == ConfirmationState::cancelled,"cancelled approval consumed");
    ConfirmationBook stale({1,1},caller); const auto old = stale.offer(target()); stale.decide(caller,{4,old,1,target(),true},{});
    another = target(); ++another.instance;
    check(!stale.consume(old,another,{}) && stale.state(old) == ConfirmationState::invalidated,"new instance used old approval");
    ConfirmationBook normalized({1,1},caller); another = target(); another.arguments = R"({"b":2,"a":1})";
    const auto canonical = normalized.offer(another); auto value = normalized.snapshot().front();
    normalized.decide(caller,{5,canonical,1,value.target,false},{});
    another.call_id = "new-id"; another.arguments = R"({ "a":1, "b":2 })";
    rejects([&] { normalized.offer(another); });
}
// 验证同批或先前批准尚未消费时，用户拒绝仍约束整个目标内的相同动作。
// 步骤：批准一个候选 → 拒绝同目标另一个候选 → 核对旧回执与现许可分离 → 保留已消费事实。
void denial_scope() {
    ConfirmationCaller caller; ConfirmationBook book({1,1},caller);
    const auto one = book.offer(target()); ConfirmationCommand approved{1,one,1,target(),true};
    const auto receipt = book.decide(caller,approved,{});
    auto other = target(); other.call_id = "sibling"; const auto two = book.offer(other);
    book.decide(caller,{2,two,1,other,false},{});
    check(book.state(one) == ConfirmationState::denied && !book.consume(one,target(),{}) && book.decide(caller,approved,{}) == receipt,
        "sibling denial left an executable approval");
    rejects([&] { book.offer(target()); });
    ConfirmationBook finished({1,1},caller); const auto done = finished.offer(target()); finished.decide(caller,{3,done,1,target(),true},{});
    check(finished.consume(done,target(),{}),"initial approval unavailable");
    const auto denied = finished.offer(other); finished.decide(caller,{4,denied,1,other,false},{});
    check(finished.state(done) == ConfirmationState::consumed,"later denial rewrote consumed action fact");
}
// 验证等待和幂等回执共用有界预算，失败不占条目，最后释放前仍计费。
void budget() {
    ConfirmationCaller caller; auto small = std::make_shared<Budget>(BudgetAmount{1,256});
    ConfirmationBook denied({1,1},caller,1,small); rejects([&] { denied.offer(target()); });
    check(!small->used().items && !denied.size(),"failed confirmation consumed budget or record");
    auto parent = std::make_shared<Budget>(BudgetAmount{1,256*1024});
    {
        ConfirmationBook book({1,1},caller,2,parent); const auto id = book.offer(target());
        check(parent->used().items == 1 && parent->used().bytes && book.waiting() == 1,"waiting confirmation uncharged");
        auto other = target(); other.call_id = "next"; rejects([&] { book.offer(other); });
        book.decide(caller,{1,id,1,target(),true},{}); check(book.consume(id,target(),{}),"budgeted approval unavailable");
        check(!book.waiting() && book.size() == 1 && parent->used().items == 1,"retained receipt released charge early");
    }
    check(!parent->used().items && !parent->used().bytes,"confirmation budget leaked");
}
// 验证计划和观察绑定改变后旧批准不能消费，拒绝也不能靠新计划版本规避。
// 步骤：固定计划及观察身份 → 改变一个维度 → 拒绝旧许可 → 核对原拒绝范围。
void changing_preconditions() {
    ConfirmationCaller caller;
    auto original = target(); original.plan_revision = 1; original.plan_step = 1; original.observation_binding = 7;
    for (unsigned mode = 0; mode < 3; ++mode) {
        ConfirmationBook book({1,1},caller); const auto id = book.offer(original);
        auto changed = original;
        if (mode == 0) ++changed.plan_revision;
        if (mode == 1) ++changed.plan_step;
        if (mode == 2) ++changed.observation_binding;
        rejects([&] { book.decide(caller,{1,id,1,changed,true},{}); });
        book.decide(caller,{1,id,1,original,true},{});
        check(!book.consume(id,changed,{}) && book.state(id) == ConfirmationState::invalidated,"changed precondition reused old approval");
        const auto summary = book.summaries()[0];
        check(summary.plan_revision == 1 && summary.plan_step == 1 && summary.observation_binding == 7,"summary rewrote original preconditions");
    }
    ConfirmationBook denied({1,1},caller); const auto id = denied.offer(original); denied.decide(caller,{1,id,1,original,false},{});
    auto changed = original; changed.plan_revision = 2; changed.observation_binding = 8; changed.call_id = "new";
    rejects([&] { denied.offer(changed); });
    ConfirmationBook malformed({1,1},caller); changed = original; changed.plan_step = 0; rejects([&] { malformed.offer(changed); });
}
}
// 只验证宿主确认值契约，不调用工具、模型或 UI。
int main() {
    try { decisions(); stops(); denial_scope(); budget(); changing_preconditions(); std::cout << "Confirmation checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
