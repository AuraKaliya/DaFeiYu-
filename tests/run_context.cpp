#include <dafeiyu/run_context.hpp>
#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <new>

namespace { bool forbid_allocations = false; }
// 为关键提交区间注入分配失败，验证动作结果投影无需新的正文分配。
void* operator new(std::size_t size) {
    if (forbid_allocations) throw std::bad_alloc();
    if (void* value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
// 归还测试替换分配器的内存，不参与业务状态。
void operator delete(void* value) noexcept { std::free(value); }
// 归还带大小信息的分配，不依赖该提示的数值。
void operator delete(void* value, std::size_t) noexcept { std::free(value); }

namespace {
using namespace dfy::agent;
using dfy::model::Role;
using dfy::runtime::OperationState;
struct AllocationGuard {
    // 进入已预留工具结果的禁止分配区间。
    AllocationGuard() { forbid_allocations = true; }
    // 无论验证是否抛错都恢复测试进程的正常分配。
    ~AllocationGuard() { forbid_allocations = false; }
};
// 报告上下文投影与真实提交边界不符。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 验证错误阶段、超量或损坏输入被拒绝。
void rejects(const auto& action) {
    try { action(); } catch (const std::logic_error&) { return; }
    throw std::runtime_error("invalid context operation accepted");
}
// 验证工具事实独立提交，后续失败和封闭不回滚已发生动作。
// 步骤：准备未提交目标 → 按原调用顺序填充结果 → 提交工具事实 → 模拟后续回答失败并封闭。
void facts() {
    const auto initial = std::make_shared<const MessageHistory>(MessageHistory{{Role::user,"earlier"},{Role::assistant,"answer"}});
    RunContext context(initial,"goal"); check(context.messages().size() == 3 && context.committed() == initial,"input committed too early");
    const dfy::model::Message assistant{Role::assistant,"",{{"a","change","{}"},{"b","unknown","{}"}}};
    context.prepare_tools(assistant,{}); rejects([&] { (void)context.messages(); }); rejects([&] { context.commit_tools(); });
    context.set_tool_result(1,{false,ToolEffect::not_applied,{},ToolError::unavailable},OperationState::failed,ToolError::none,false);
    check(context.committed() == initial,"partial results committed");
    context.set_tool_result(0,{true,ToolEffect::applied,"changed",ToolError::none},OperationState::cancelled);
    rejects([&] { context.set_tool_result(0,{},OperationState::failed); }); context.commit_tools();
    const auto committed = context.committed();
    check(committed->size() == 6 && (*committed)[4].tool_call_id == "a" && (*committed)[5].tool_call_id == "b" &&
        (*committed)[4].content.find("status=cancelled; effect=applied") == 0 && (*committed)[5].content.find("status=rejected") == 0,
        "tool pairing, rejection or cancelled effect lost");
    rejects([&] { context.finish_text(std::string(1024*1024,'x')); }); context.seal();
    check(context.committed() == committed && initial->size() == 2,"later failure rolled back facts or changed old snapshot");
}
// 验证动作前容量预留及多批次投影，非法结果不会被当作完成。
// 步骤：拒绝无法预留的批次 → 拒绝损坏结果 → 提交两批同局部 ID 调用 → 提交最终文本。
void boundaries() {
    const auto empty = std::make_shared<const MessageHistory>();
    const dfy::model::Message assistant{Role::assistant,"",{{"same","read","{}"}}};
    RunContext full(empty,std::string(1024*1024-100,'x'));
    rejects([&] { full.prepare_tools(assistant,{}); });
    check(full.committed() == empty && full.messages().size() == 1,"failed reservation changed context");
    RunContext context(empty,"small");
    for (unsigned i = 0; i < 2; ++i) {
        context.prepare_tools(assistant,{});
        rejects([&] { context.set_tool_result(0,{true,ToolEffect::not_applied,std::string(1,static_cast<char>(0xff)),ToolError::none},OperationState::succeeded); });
        const ToolResult outcome{true,ToolEffect::not_applied,std::string(64*1024,'x'),ToolError::none};
        {
            AllocationGuard guard;
            context.set_tool_result(0,outcome,OperationState::succeeded);
            context.commit_tools();
        }
    }
    context.finish_text("done"); check(context.committed()->size() == 6,"multi-step final history lost messages");
    rejects([&] { context.prepare_tools(assistant,{}); });
}
}
// 只验证固定消息和工具结果投影，不调用模型或执行工具。
int main() {
    try { facts(); boundaries(); std::cout << "Run context checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
