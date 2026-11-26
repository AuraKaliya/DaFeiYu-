#pragma once
#include <dafeiyu/execution_policy.hpp>
#include <dafeiyu/run_context.hpp>
#include <dafeiyu/tool_executor.hpp>
#include <dafeiyu/confirmation.hpp>
#include <dafeiyu/run_journal.hpp>
#include <dafeiyu/run_capture.hpp>

namespace dfy::agent {
struct BatchToolSnapshot {
    std::optional<std::string> tool_id;
    AttemptId attempt;
    runtime::DomainId candidate = 0;
    std::uint64_t revision = 0;
    std::string call_id, name;
    bool accepted = false;
    runtime::DomainId invocation = 0;
    std::optional<PolicyDecision> before, after;
    runtime::OperationState result = runtime::OperationState::failed, exit = runtime::OperationState::succeeded;
    ToolEffect effect = ToolEffect::not_applied;
    ToolError error = ToolError::unavailable, fault = ToolError::none;
    runtime::DomainId confirmation = 0;
    std::optional<ConfirmationState> confirmation_state;
    std::string rejection;
};
// 一次助手工具批次的协调器；宿主须保持上下文、执行器及本对象到结果提交完成。
class ToolBatch {
  public:
    // 动作前预留回填并检查策略；确认账本须存活到批次销毁，等待时不接纳工具。
    ToolBatch(RunContext& context, ToolExecutor& executor, const model::Message& assistant,
        const ToolSnapshot& tools, ExecutionPolicy policy, PolicyContext control, PolicyExtension extension = {}, ConfirmationBook* confirmations = nullptr,
        RunJournal* journal = nullptr, ToolCaptureSink capture = {});
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
    // 返回完整结果后的进展指纹；参数仅忽略 JSON 排版差异，未完成批次拒绝读取。
    std::uint64_t progress_fingerprint() const;
    // 返回只读值快照，不把工作、参数或输出持有交给观察者。
    std::vector<BatchToolSnapshot> snapshot() const;
    // 复制已实际退出候选的原工具回执正文；尚未退出或未接纳返回空，不推进执行。
    std::optional<ToolResult> result(runtime::DomainId candidate) const;
  private:
    struct Entry { BatchToolSnapshot metadata; model::ToolCall call; std::shared_ptr<ToolRequest> request; std::optional<TraceFact> last; bool filled = false; };
    // 按阶段发布工具实际参数，记录异常不改变接纳或结果。
    void capture(const Entry& entry, ToolCapturePhase phase) noexcept;
    // 在原工具状态发生变化时记录无正文事实，重复推进不追加相同状态。
    void publish(std::size_t index, TraceFactKind kind = TraceFactKind::tool_changed) noexcept;
    // 记录候选控制决定，不替代执行入口的权限或参数检查。
    void publish_policy(std::size_t index, ControlPoint point, PolicyDecision decision) noexcept;
    // 重新核对全部候选与确认状态；任一等待使整个未接纳批次让出宿主。
    bool review(const PolicyContext& current);
    // 确认全部就绪后消费批准并接纳，仍不在该边界执行工具工厂。
    void admit(const PolicyContext& current);
    // 为未执行候选填入拒绝事实，并使尚未消费的确认失效。
    void reject(std::size_t index, ConfirmationState reason = ConfirmationState::invalidated,
        std::string detail = R"({"stage":"admission","reason":"unavailable"})");
    RunContext& context_;
    ToolExecutor& executor_;
    const ToolSnapshot tools_;
    ConfirmationBook* const confirmations_;
    RunJournal* const journal_;
    const ExecutionPolicy policy_;
    const PolicyContext bound_;
    PolicyExtension extension_;
    const ToolCaptureSink capture_;
    std::vector<Entry> entries_;
    PolicyDecision continuation_{PolicyVerdict::allow,PolicyReason::none};
    std::uint64_t fingerprint_ = UINT64_C(14695981039346656037);
    bool done_ = false, advancing_ = false, admitted_ = false;
};
}
