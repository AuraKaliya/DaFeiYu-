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
}
// 只验证宿主确认值契约，不调用工具、模型或 UI。
int main() {
    try { decisions(); stops(); std::cout << "Confirmation checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
