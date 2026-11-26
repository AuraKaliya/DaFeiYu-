#include <dafeiyu/tool_batch.hpp>
#include <iostream>

namespace {
using namespace dfy::agent;
using namespace dfy::runtime;
// 报告批次等待、确认和实际调用之间的边界错误。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 创建固定运行上下文，时钟由测试显式推进。
PolicyContext control() { return {{{{1,1},1},1},1,1,RunOrigin::user_request,1,0,{},ToolTime{}+std::chrono::seconds(1)}; }
// 验证等候不接纳动作，批准只在下一推进执行，拒绝及失效均保持零动作。
// 步骤：准备真实目录和批次 → 保持等待并提交决定 → 注入停止或撤销 → 检查动作及配对事实。
void scenarios() {
    for (unsigned mode = 0; mode < 7; ++mode) {
        unsigned calls = 0; ToolCatalog catalog;
        const auto registration = catalog.add({"fixture/change",1,1,{"change","fixture",R"({"type":"object","properties":{},"additionalProperties":false})"},true,false},
            [&](const auto&) { ++calls; return completed_tool({true,ToolEffect::applied,"changed",ToolError::none}); });
        const std::vector<std::string> ids{"fixture/change"}; const auto tools = catalog.snapshot(ids,true);
        ConfirmationCaller caller; ConfirmationBook confirmations({1,1},caller); ToolExecutor executor({1,1},tools);
        RunContext context(std::make_shared<const MessageHistory>(),"goal"); PolicySettings settings; settings.confirm_changes = true;
        ToolBatch batch(context,executor,{dfy::model::Role::assistant,"",{{"call","change","{}"}}},tools,ExecutionPolicy({1,1},1,settings),control(),{},&confirmations);
        check(!executor.size() && !batch.advance(control()) && !calls && !executor.size(),"waiting acquired a tool or executed");
        const auto value = confirmations.snapshot().front(); ConfirmationCommand command{1,value.id,value.revision,value.target,mode != 1};
        confirmations.decide(caller,command,{}); auto current = control();
        if (mode == 2) current.cancellation = CancelReason::user;
        if (mode == 3) current.now = current.deadline;
        if (mode == 4) catalog.remove(registration);
        if (mode == 5) ++current.goal_revision;
        batch.advance(current); check(!calls,"approval executed inline");
        if (mode == 6) current.cancellation = CancelReason::user;
        for (unsigned tick = 0; !batch.done() && tick < 4; ++tick) batch.advance(current);
        check(batch.done() && calls == (mode == 0 ? 1u : 0u) && context.committed()->size() == 3,"confirmation boundary failed");
        const auto snapshot = batch.snapshot().front();
        check(snapshot.confirmation == value.id && snapshot.effect == (mode == 0 ? ToolEffect::applied : ToolEffect::not_applied),"confirmation lost tool facts");
    }
}
// 验证混合批次整体等待，先出现的已允许读取也不能穿过确认边界。
void mixed_wait() {
    unsigned calls = 0; ToolCatalog catalog;
    const auto factory = [&](const auto&) { ++calls; return completed_tool({true,ToolEffect::not_applied,"read",ToolError::none}); };
    const dfy::model::ToolDefinition read{"read","fixture",R"({"type":"object","properties":{},"additionalProperties":false})"};
    catalog.add({"fixture/read",1,1,read,false,true},factory);
    auto change = read; change.name = "change"; catalog.add({"fixture/change",1,1,change,true,false},factory);
    const std::vector<std::string> ids{"fixture/read","fixture/change"}; const auto tools = catalog.snapshot(ids,true);
    ConfirmationCaller caller; ConfirmationBook book({1,1},caller); ToolExecutor executor({1,1},tools);
    RunContext context(std::make_shared<const MessageHistory>(),"goal"); PolicySettings settings; settings.confirm_changes = true;
    ToolBatch batch(context,executor,{dfy::model::Role::assistant,"",{{"one","read","{}"},{"two","change","{}"}}},tools,
        ExecutionPolicy({1,1},1,settings),control(),{},&book);
    for (unsigned i = 0; i < 3; ++i) check(!batch.advance(control()) && !calls && !executor.size(),"mixed batch bypassed wait");
    const auto value = book.snapshot().front(); book.decide(caller,{1,value.id,1,value.target,false},{});
    for (unsigned i = 0; !batch.done() && i < 4; ++i) batch.advance(control());
    check(batch.done() && calls == 1 && !batch.snapshot()[1].accepted,"denial suppressed allowed read or executed denied tool");
    RunContext abandoned(std::make_shared<const MessageHistory>(),"goal"); ConfirmationBook cleanup({1,1},caller);
    {
        ToolBatch waiting(abandoned,executor,{dfy::model::Role::assistant,"",{{"three","change","{}"}}},tools,
            ExecutionPolicy({1,1},1,settings),control(),{},&cleanup);
    }
    check(cleanup.snapshot().front().state == ConfirmationState::cancelled && calls == 1,"destroyed batch left live confirmation");
}
// 比较两次同因拒绝的进展指纹，回执保留各自的原审批号。
// 步骤：两次签发独立审批 → 按原号拒绝并核对真实回执 → 比较语义进展。
void repeated_denial() {
    ToolCatalog catalog;
    catalog.add({"fixture/change",1,1,{"change","fixture",R"({"type":"object","properties":{},"additionalProperties":false})"},true,false});
    const auto tools = catalog.snapshot(std::vector<std::string>{"fixture/change"},true);
    std::optional<std::uint64_t> first;
    DomainId previous = 0;
    for (const auto id : {"one","two"}) {
        ConfirmationCaller caller; ConfirmationBook book({1,1},caller); ToolExecutor executor({1,1},tools);
        RunContext context(std::make_shared<const MessageHistory>(),"goal"); PolicySettings settings; settings.confirm_changes = true;
        ToolBatch batch(context,executor,{dfy::model::Role::assistant,"",{{id,"change","{}"}}},tools,ExecutionPolicy({1,1},1,settings),control(),{},&book);
        const auto decision = book.snapshot().at(0); book.decide(caller,{1,decision.id,decision.revision,decision.target,false},{});
        for (unsigned i=0; !batch.done() && i<4; ++i) batch.advance(control());
        const auto& output = context.committed()->back().content;
        check(!executor.size() && previous != decision.id && output.find("\"approval\":"+std::to_string(decision.id)) != std::string::npos &&
            output.find("\"approvalState\":\"denied\"") != std::string::npos,"denial lost its own receipt");
        const auto key = batch.progress_fingerprint();
        check(!first || *first == key,"new approval identity falsely counted as progress");
        first = key; previous = decision.id;
    }
}
// 验证模型同批重复提出的同一动作不能通过分别批准和拒绝绕开拒绝范围。
void duplicate_candidates() {
    unsigned calls = 0; ToolCatalog catalog;
    catalog.add({"fixture/change",1,1,{"change","fixture",R"({"type":"object","properties":{},"additionalProperties":false})"},true,false},
        [&](const auto&) { ++calls; return completed_tool({true,ToolEffect::applied,"changed",ToolError::none}); });
    const std::vector<std::string> ids{"fixture/change"}; const auto tools = catalog.snapshot(ids,true);
    ConfirmationCaller caller; ConfirmationBook book({1,1},caller); ToolExecutor executor({1,1},tools);
    RunContext context(std::make_shared<const MessageHistory>(),"goal"); PolicySettings settings; settings.confirm_changes = true;
    ToolBatch batch(context,executor,{dfy::model::Role::assistant,"",{{"one","change","{}"},{"two","change","{}"}}},tools,
        ExecutionPolicy({1,1},1,settings),control(),{},&book);
    const auto values = book.snapshot(); book.decide(caller,{1,values[0].id,1,values[0].target,true},{});
    book.decide(caller,{2,values[1].id,1,values[1].target,false},{});
    for (unsigned i = 0; !batch.done() && i < 4; ++i) batch.advance(control());
    check(batch.done() && !calls && !executor.size(),"duplicate call bypassed same-goal denial");
}
}
// 使用本地工具工厂验证确认，不调用模型。
int main() {
    try { scenarios(); mixed_wait(); repeated_denial(); duplicate_candidates(); std::cout << "Tool confirmation checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
