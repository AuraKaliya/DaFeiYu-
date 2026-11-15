#include <dafeiyu/tool_batch.hpp>
#include <stdexcept>

namespace dfy::agent {
// 准备有界工具批次，任何拒绝都产生配对回执而不执行该候选动作。
// 步骤：检查总调用预算 → 准备元数据和完整上下文 → 解析并裁定各候选 → 接纳或填写拒绝结果。
ToolBatch::ToolBatch(RunContext& context, ToolExecutor& executor, const model::Message& assistant,
    const ToolSnapshot& tools, ExecutionPolicy policy, PolicyContext control, PolicyExtension extension)
    : context_(context), executor_(executor), policy_(std::move(policy)), bound_(control), extension_(std::move(extension)) {
    const auto count = assistant.tool_calls.size(), limit = policy_.settings().tool_limit;
    if (!count || control.tool_requests > limit || count > limit-control.tool_requests) throw std::length_error("batch: tool limit exhausted");
    entries_.resize(count);
    for (std::size_t index = 0; index < count; ++index) {
        entries_[index].metadata.call_id = assistant.tool_calls[index].id; entries_[index].metadata.name = assistant.tool_calls[index].name;
    }
    context_.prepare_tools(assistant,tools.definitions());
    for (std::size_t index = 0; index < count; ++index) {
        auto& entry = entries_[index]; auto current = control; current.tool_requests += index;
        try {
            const auto& call = assistant.tool_calls[index]; const auto selected = tools.resolve(call.name,call.arguments);
            entry.metadata.before = restrict_policy(policy_.before_tool(current,selected),extension_,ControlPoint::tool_start,current);
            if (entry.metadata.before->verdict == PolicyVerdict::allow) {
                entry.request = executor_.submit(current.attempt,call); entry.metadata.accepted = true;
                entry.metadata.invocation = entry.request->invocation().invocation;
            }
        } catch (...) {}
        if (!entry.request) {
            context_.set_tool_result(index,{false,ToolEffect::not_applied,{},ToolError::unavailable},runtime::OperationState::failed,ToolError::none,false);
            entry.filled = true;
        }
    }
}
// 丢弃协调器不释放在途执行器责任，只明确请求尚未退出的工作停止。
ToolBatch::~ToolBatch() {
    if (!done_) for (const auto& entry : entries_) if (entry.request) entry.request->request_cancel(runtime::CancelReason::owner_stop);
}
// 推进并接收原结果，取消或策略停止仍保留真实效果并完成回填。
// 步骤：保护入口并重查范围 → 按需停止后续接纳 → 推进实际工作 → 填写终态和效果 → 发布完整上下文。
bool ToolBatch::advance(const PolicyContext& current) {
    using namespace runtime;
    if (advancing_) throw std::logic_error("batch: reentrant advance");
    if (done_) return true;
    advancing_ = true;
    try {
        auto decision = policy_.check_context(current);
        if (current.attempt != bound_.attempt) decision = {PolicyVerdict::deny,PolicyReason::invalid_context};
        if (decision.verdict != PolicyVerdict::allow) {
            continuation_ = decision;
            const bool valid_cancel = current.cancellation == CancelReason::user || current.cancellation == CancelReason::owner_stop || current.cancellation == CancelReason::deadline;
            const auto reason = valid_cancel ? current.cancellation :
                decision.reason == PolicyReason::deadline ? CancelReason::deadline : CancelReason::owner_stop;
            executor_.close(reason);
        }
        executor_.advance(); bool complete = true;
        for (std::size_t index = 0; index < entries_.size(); ++index) {
            auto& entry = entries_[index]; if (entry.filled) continue;
            if (entry.request->exit()->state() == OperationState::pending) { complete = false; continue; }
            entry.metadata.after = restrict_policy(policy_.after_tool(current,*entry.request),extension_,ControlPoint::tool_result,current);
            if (entry.metadata.after->verdict != PolicyVerdict::allow) {
                continuation_ = *entry.metadata.after; executor_.close(CancelReason::owner_stop);
            }
            context_.set_tool_result(index,entry.request->outcome(),entry.request->operation()->state(),entry.request->fault());
            entry.filled = true;
        }
        if (complete) { context_.commit_tools(); done_ = true; }
    } catch (...) { advancing_ = false; throw; }
    advancing_ = false; return done_;
}
// 从原请求读取最新状态形成值副本，不保存任何原生工作引用。
std::vector<BatchToolSnapshot> ToolBatch::snapshot() const {
    std::vector<BatchToolSnapshot> result; result.reserve(entries_.size());
    for (const auto& entry : entries_) {
        auto value = entry.metadata;
        if (entry.request) {
            value.result = entry.request->operation()->state(); value.exit = entry.request->exit()->state(); value.effect = entry.request->outcome().effect;
            value.error = entry.request->outcome().error; value.fault = entry.request->fault();
        }
        result.push_back(std::move(value));
    }
    return result;
}
}
