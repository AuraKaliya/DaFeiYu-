#include <dafeiyu/tool_catalog.hpp>
#include <iostream>
#include <stdexcept>

namespace {
using namespace dfy::agent;
using namespace dfy::runtime;
// 报告授权执行与预期不一致。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 确认非法候选调用在工具执行之前被拒绝。
void rejects(const auto& action) {
    try { action(); } catch (const std::logic_error&) { return; }
    throw std::runtime_error("unauthorized invocation accepted");
}
// 验证预设候选调用被正确选择、调用和返回，撤销会阻止已经准备的未开始工作。
// 步骤：注册受控能力 → 拒绝错误选择和参数 → 实际执行一次 → 撤销后保持零追加调用。
void invocation() {
    unsigned calls = 0; ToolCatalog catalog(2);
    ToolDescription description{"fixture/read",9,1,{"read","read",R"({"type":"object","properties":{},"additionalProperties":false})"}};
    const auto registration = catalog.add(description,[&](const ToolInvocation& input) {
        check(input.attempt.step.run == RunId{7,1} && input.call.id == "call" && input.call.arguments == "{}","wrong invocation identity or arguments");
        ++calls; return completed_tool({true,ToolEffect::not_applied,"value",ToolError::none});
    });
    const std::vector<std::string> ids{"fixture/read"}; const auto snapshot = catalog.snapshot(ids);
    const AttemptId attempt{StepId{RunId{7,1},1},1};
    rejects([&] { (void)snapshot.prepare(attempt,{"call","unknown","{}"}); });
    rejects([&] { (void)snapshot.prepare(attempt,{"call","read",R"({"extra":1})"}); });
    check(!calls,"invalid candidate called the implementation");
    auto driver = snapshot.prepare(attempt,{"call","read","{}"}); const auto request = driver->request();
    check(!calls && request->operation()->state() == OperationState::pending,"preparation executed the tool");
    driver->advance(); driver->advance();
    check(calls == 1 && request->operation()->state() == OperationState::succeeded && request->outcome().output == "value" &&
        request->exit()->state() == OperationState::succeeded,"tool did not execute exactly once or lost result");
    auto revoked = snapshot.prepare(attempt,{"later","read","{}"}); catalog.enable(registration,false); revoked->advance();
    check(calls == 1 && revoked->request()->operation()->state() == OperationState::failed &&
        revoked->request()->outcome().effect == ToolEffect::not_applied && revoked->request()->outcome().error == ToolError::unavailable,
        "revoked prepared work still called implementation");
    catalog.remove(registration); description.instance = 10;
    catalog.add(description,[&](const auto&) { ++calls; return completed_tool({true,ToolEffect::not_applied,"new",ToolError::none}); });
    rejects([&] { (void)snapshot.prepare(attempt,{"stale","read","{}"}); });
    check(calls == 1,"old snapshot was redirected to a new instance");
    description.id = "unbound"; description.model.name = "unbound"; catalog.add(description);
    const std::vector<std::string> unbound_ids{"unbound"}; const auto unbound = catalog.snapshot(unbound_ids);
    check(unbound.definitions().empty(),"unbound description exposed to model");
    rejects([&] { (void)unbound.prepare(attempt,{"call","unbound","{}"}); });
}
}
// 执行固定工具调用夹具，不调用任何模型。
int main() {
    try { invocation(); std::cout << "Tool invocation checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
