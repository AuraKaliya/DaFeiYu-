#pragma once
#include <dafeiyu/execution_policy.hpp>
#include <dafeiyu/run_context.hpp>
#include <dafeiyu/tool_executor.hpp>

namespace dfy::agent {
struct BatchToolSnapshot {
    std::string call_id, name;
    bool accepted = false;
    runtime::DomainId invocation = 0;
    std::optional<PolicyDecision> before, after;
    runtime::OperationState result = runtime::OperationState::failed, exit = runtime::OperationState::succeeded;
    ToolEffect effect = ToolEffect::not_applied;
    ToolError error = ToolError::unavailable, fault = ToolError::none;
};
// 一次助手工具批次的协调器；宿主须保持上下文、执行器及本对象到结果提交完成。
class ToolBatch {
  public:
    // 动作前预留所有回填空间，逐项检查策略并接纳，构造期间不执行工具。
    ToolBatch(RunContext& context, ToolExecutor& executor, const model::Message& assistant,
        const ToolSnapshot& tools, ExecutionPolicy policy, PolicyContext control, PolicyExtension extension = {});
    // 未完成时仅请求取消，实际工作责任仍由执行器保留，不提交伪造结果。
    ~ToolBatch();
    // 禁止复制批次结果回填责任。
    ToolBatch(const ToolBatch&) = delete;
    // 禁止赋值或移动覆盖正在运行的批次。
    ToolBatch& operator=(const ToolBatch&) = delete;
    // 先检查当前控制边界再推进工作，全部实际退出后提交完整工具上下文。
    bool advance(const PolicyContext& current);
    // 返回是否已提交整个批次，不把部分业务完成当作整批退出。
    bool done() const noexcept { return done_; }
    // 返回结果后对下一调度的约束，不改写已记录工具事实。
    PolicyDecision continuation() const noexcept { return continuation_; }
    // 返回只读值快照，不把工作、参数或输出持有交给观察者。
    std::vector<BatchToolSnapshot> snapshot() const;
  private:
    struct Entry { BatchToolSnapshot metadata; std::shared_ptr<ToolRequest> request; bool filled = false; };
    RunContext& context_;
    ToolExecutor& executor_;
    const ExecutionPolicy policy_;
    const PolicyContext bound_;
    const PolicyExtension extension_;
    std::vector<Entry> entries_;
    PolicyDecision continuation_{PolicyVerdict::allow,PolicyReason::none};
    bool done_ = false, advancing_ = false;
};
}
