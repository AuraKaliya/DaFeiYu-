#include <dafeiyu/tool_batch.hpp>
#include <stdexcept>

namespace dfy::agent {
// 准备有界工具批次，任何拒绝都产生配对回执而不执行该候选动作。
// 步骤：检查总调用预算 → 准备元数据和完整上下文 → 裁定候选及确认 → 全部就绪才接纳。
ToolBatch::ToolBatch(RunContext& context, ToolExecutor& executor, const model::Message& assistant,
    const ToolSnapshot& tools, ExecutionPolicy policy, PolicyContext control, PolicyExtension extension, ConfirmationBook* confirmations)
    : context_(context), executor_(executor), tools_(tools), confirmations_(confirmations), policy_(std::move(policy)), bound_(control), extension_(std::move(extension)) {
    const auto count = assistant.tool_calls.size(), limit = policy_.settings().tool_limit;
    if (!count || control.tool_requests > limit || count > limit-control.tool_requests) throw std::length_error("batch: tool limit exhausted");
    entries_.resize(count);
    for (std::size_t index = 0; index < count; ++index) {
        entries_[index].metadata.call_id = assistant.tool_calls[index].id; entries_[index].metadata.name = assistant.tool_calls[index].name;
        entries_[index].call = assistant.tool_calls[index];
    }
    context_.prepare_tools(assistant,tools.definitions());
    if (review(control)) admit(control);
}
// 为拒绝候选记录零动作，不改写已经消费或发生效果的确认。
void ToolBatch::reject(std::size_t index, ConfirmationState reason) {
    auto& entry = entries_[index];
    if (entry.metadata.confirmation) confirmations_->invalidate(entry.metadata.confirmation,reason);
    context_.set_tool_result(index,{false,ToolEffect::not_applied,{},ToolError::unavailable},runtime::OperationState::failed,ToolError::none,false);
    entry.metadata.result = runtime::OperationState::failed; entry.metadata.exit = runtime::OperationState::succeeded; entry.filled = true;
}
// 将候选和决定维持为值记录，等待不能占用执行顺序或工具工作资源。
// 步骤：读取原确认状态 → 重新解析授权与策略 → 登记必要确认 → 汇总批次是否可以接纳。
bool ToolBatch::review(const PolicyContext& current) {
    bool ready = true;
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        auto& entry = entries_[index]; if (entry.filled) continue;
        try {
            if (entry.metadata.confirmation) {
                const auto state = confirmations_->state(entry.metadata.confirmation);
                if (state != ConfirmationState::waiting && state != ConfirmationState::approved) { reject(index); continue; }
            }
            const auto selected = tools_.resolve(entry.call.name,entry.call.arguments);
            auto candidate = current; candidate.tool_requests = bound_.tool_requests+index;
            entry.metadata.before = restrict_policy(policy_.before_tool(candidate,selected),extension_,ControlPoint::tool_start,candidate);
            const auto decision = *entry.metadata.before;
            if (decision.verdict == PolicyVerdict::deny) { reject(index); continue; }
            if (decision.verdict == PolicyVerdict::wait) {
                if (!confirmations_ || decision.reason != PolicyReason::confirmation_required) { reject(index); continue; }
                if (!entry.metadata.confirmation) entry.metadata.confirmation = confirmations_->offer(confirmation_target(candidate,selected,entry.call));
            }
            if (entry.metadata.confirmation && confirmations_->state(entry.metadata.confirmation) == ConfirmationState::waiting) ready = false;
            entry.metadata.result = entry.metadata.exit = runtime::OperationState::pending;
        } catch (...) { reject(index); }
    }
    return ready;
}
// 全部等待已决定后重新绑定目标并提交；失败反馈不能恢复一次已消费批准。
// 步骤：逐项核对精确批准 → 复用工具入口最终授权及预算校验 → 保存回执或零动作拒绝。
void ToolBatch::admit(const PolicyContext& current) {
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        auto& entry = entries_[index]; if (entry.filled) continue;
        try {
            if (entry.metadata.confirmation && !confirmations_->consume(entry.metadata.confirmation,
                confirmation_target(current,tools_.resolve(entry.call.name,entry.call.arguments),entry.call),current.now)) { reject(index); continue; }
            entry.request = executor_.submit(current.attempt,entry.call); entry.metadata.accepted = true;
            entry.metadata.invocation = entry.request->invocation().invocation;
        } catch (...) { reject(index); }
    }
    admitted_ = true;
}
// 丢弃协调器只取消未消费确认及在途工作，不释放执行器的实际退出责任。
ToolBatch::~ToolBatch() {
    if (!done_) for (const auto& entry : entries_) {
        if (entry.metadata.confirmation) confirmations_->invalidate(entry.metadata.confirmation,ConfirmationState::cancelled);
        if (entry.request) entry.request->request_cancel(runtime::CancelReason::owner_stop);
    }
}
// 推进并接收原结果，取消或策略停止仍保留真实效果并完成回填。
// 步骤：保护入口并重查范围 → 按需停止后续接纳 → 推进实际工作 → 填写终态和效果 → 发布完整上下文。
bool ToolBatch::advance(const PolicyContext& current) {
    using namespace runtime;
    if (advancing_) throw std::logic_error("batch: reentrant advance");
    if (done_) return true;
    advancing_ = true;
    try {
        if (confirmations_) confirmations_->advance(current.now,current.cancellation);
        auto decision = policy_.check_context(current);
        if (current.attempt != bound_.attempt) decision = {PolicyVerdict::deny,PolicyReason::invalid_context};
        if (decision.verdict != PolicyVerdict::allow) {
            continuation_ = decision;
            const bool valid_cancel = current.cancellation == CancelReason::user || current.cancellation == CancelReason::owner_stop || current.cancellation == CancelReason::deadline;
            const auto reason = valid_cancel ? current.cancellation :
                decision.reason == PolicyReason::deadline ? CancelReason::deadline : CancelReason::owner_stop;
            executor_.close(reason);
            if (!admitted_) {
                for (std::size_t index = 0; index < entries_.size(); ++index) if (!entries_[index].filled)
                    reject(index,reason == CancelReason::deadline ? ConfirmationState::expired : ConfirmationState::cancelled);
                admitted_ = true;
            }
        }
        if (!admitted_) {
            // 确认后的接纳与执行分属两次推进，让运行取消和期限在真正动作前再次裁定。
            if (review(current)) admit(current);
            advancing_ = false; return false;
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
        if (value.confirmation) value.confirmation_state = confirmations_->state(value.confirmation);
        if (entry.request) {
            value.result = entry.request->operation()->state(); value.exit = entry.request->exit()->state(); value.effect = entry.request->outcome().effect;
            value.error = entry.request->outcome().error; value.fault = entry.request->fault();
        }
        result.push_back(std::move(value));
    }
    return result;
}
}
