#include <dafeiyu/action_observations.hpp>
#include <dafeiyu/run_context.hpp>
#include <iostream>

namespace {
using namespace dfy::agent;
using namespace dfy::runtime;
// 报告新观察的身份、时间或上下文边界错误。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 核对损坏更新不会产生部分上下文。
void rejects(const auto& action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid observation refresh accepted");
}
// 创建版本明确的宿主状态，内容变化不改变字段契约。
std::shared_ptr<const StructValue> payload(std::uint32_t version = 1) {
    const FieldDescriptor fields[] = {{"value",ValueKind::uint64,0}};
    auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("fixture.refresh",version,fields,{1,64}));
    const NamedValue values[] = {{"value",Value::from_uint64(1)}};
    return std::make_shared<const StructValue>(StructValue::create(type,values,64));
}
// 验证完整更新、对象身份、采样时间和 schema，旧值不能被重新包装为新依据。
// 步骤：固定两个对象 → 拒绝局部更新 → 接纳全部新采样 → 拒绝跨集合、换代及 schema 变化。
void binding() {
    const ObservationTime zero{}; const auto later = zero+std::chrono::milliseconds(20);
    ObservationSource source(1,ObservationTrust::host_verified); ObservationInbox inbox({source.id()}), foreign({source.id()});
    const auto sample = [&](std::string id,std::uint64_t revision,std::uint64_t generation = 1,std::uint32_t schema = 1) {
        return source.capture({std::move(id),generation},revision,revision == 1 ? zero : later,zero+std::chrono::seconds(1),payload(schema));
    };
    inbox.accept(sample("a",1),zero); inbox.accept(sample("b",1),zero);
    const auto guards = [&](const ObservationInbox& values) {
        return std::vector<ObservationGuard>{values.guard(values.latest(source.id(),"a"),later),values.guard(values.latest(source.id(),"b"),later)};
    };
    const ActionObservations original(guards(inbox));
    const ActionObservations repackaged(guards(inbox)); check(!repackaged.succeeds(original,later,zero),"old samples repackaged as fresh");
    inbox.accept(sample("a",2),later); check(!ActionObservations(guards(inbox)).succeeds(original,later,zero),"partial observation set refreshed");
    inbox.accept(sample("b",2),later); auto reversed = guards(inbox); std::swap(reversed[0],reversed[1]);
    const ActionObservations current(reversed);
    check(current.succeeds(original,later,later) && !current.succeeds(original,later,later+std::chrono::nanoseconds(1)),"time boundary or order-independent matching failed");
    foreign.accept(sample("a",2),later); foreign.accept(sample("b",2),later);
    const auto refresh_a = *foreign.refresh_ticket(source.id(),"a"), refresh_b = *foreign.refresh_ticket(source.id(),"b");
    foreign.refresh(refresh_a,sample("a",3),later); foreign.refresh(refresh_b,sample("b",3),later);
    check(!ActionObservations(guards(foreign)).succeeds(original,later,zero),"foreign inbox used same source names");
    inbox.accept(sample("a",3,1,2),later); check(!ActionObservations(guards(inbox)).succeeds(original,later,zero),"schema change accepted");
    inbox.accept(sample("a",1,2),later); check(!ActionObservations(guards(inbox)).succeeds(original,later,zero),"object generation changed silently");
}
// 验证刷新不抹去动作事实，失败或未提交刷新不改变已提交历史。
// 步骤：准备原动作批次 → 拒绝在途刷新 → 提交真实效果 → 追加新数据并验证失败回滚。
void context() {
    const auto empty = std::make_shared<const MessageHistory>(); RunContext context(empty,"goal",{"old"});
    context.prepare_tools({dfy::model::Role::assistant,"",{{"call","change","{}"}}},{});
    rejects([&] { context.refresh_observations(1,{"new"}); });
    context.set_tool_result(0,{true,ToolEffect::applied,"changed",ToolError::none},OperationState::succeeded); context.commit_tools();
    const auto old = context.committed(); context.refresh_observations(2,{"new-a","new-b"});
    check(context.committed() == old && context.messages().size() == old->size()+2 && (*old)[3].content.find("effect=applied") != std::string::npos,
        "refresh committed early or lost action facts");
    const auto last = context.messages().back().content;
    rejects([&] { context.refresh_observations(3,{"valid",std::string(1,static_cast<char>(0xff))}); });
    rejects([&] { context.refresh_observations(0,{"new"}); });
    check(context.messages().back().content == last,"failed refresh published partial data"); context.seal();
    check(context.committed() == old,"sealing discarded original action facts");
}
}
// 只验证固定观察和上下文值，不调用模型或执行工具。
int main() {
    try { binding(); context(); std::cout << "Action observation refresh checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
