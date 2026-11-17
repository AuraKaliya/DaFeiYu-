#include <dafeiyu/tool_batch.hpp>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace dfy::agent {
namespace {
// 按文本及字段边界累积轻量指纹，只用于无进展停止，不作为安全身份或幂等凭证。
void add_fingerprint(std::uint64_t& hash, std::string_view text) noexcept {
    for (const unsigned char byte : text) { hash ^= byte; hash *= UINT64_C(1099511628211); }
    hash ^= 255;
}
}
// 准备有界工具批次，任何拒绝都产生配对回执而不执行该候选动作。
// 步骤：检查总调用预算 → 准备元数据和完整上下文 → 裁定候选及确认 → 全部就绪才接纳。
ToolBatch::ToolBatch(RunContext& context, ToolExecutor& executor, const model::Message& assistant,
    const ToolSnapshot& tools, ExecutionPolicy policy, PolicyContext control, PolicyExtension extension, ConfirmationBook* confirmations, RunJournal* journal)
    : context_(context), executor_(executor), tools_(tools), confirmations_(confirmations), journal_(journal), policy_(std::move(policy)), bound_(control), extension_(std::move(extension)) {
    const auto count = assistant.tool_calls.size(), limit = policy_.settings().tool_limit;
    if (!count || control.tool_requests > limit || count > limit-control.tool_requests) throw std::length_error("batch: tool limit exhausted");
    entries_.resize(count);
    for (std::size_t index = 0; index < count; ++index) {
        entries_[index].metadata.call_id = assistant.tool_calls[index].id; entries_[index].metadata.name = assistant.tool_calls[index].name;
        entries_[index].call = assistant.tool_calls[index];
        entries_[index].metadata.attempt = control.attempt; entries_[index].metadata.candidate = runtime::next_domain();
        entries_[index].metadata.result = entries_[index].metadata.exit = runtime::OperationState::pending;
        add_fingerprint(fingerprint_,assistant.tool_calls[index].name);
        // 仅规范化已经通过有界 schema 的候选；未知或非法候选仍按原文本比较，不扩大解析深度。
        try {
            tools.resolve(assistant.tool_calls[index].name,assistant.tool_calls[index].arguments);
            add_fingerprint(fingerprint_,nlohmann::json::parse(assistant.tool_calls[index].arguments).dump());
        } catch (const std::invalid_argument&) { add_fingerprint(fingerprint_,assistant.tool_calls[index].arguments); }
        catch (const std::length_error&) { add_fingerprint(fingerprint_,assistant.tool_calls[index].arguments); }
    }
    context_.prepare_tools(assistant,tools.definitions());
    for (std::size_t index = 0; index < count; ++index) publish(index,TraceFactKind::tool_candidate);
    if (review(control)) admit(control);
}
// 在原状态变化后记录投影，结果成功与资源仍待退出可以同时出现。
// 步骤：读取原工具或候选状态 → 跳过重复投影 → 签发来源修订并写入有界记录。
void ToolBatch::publish(std::size_t index, TraceFactKind kind) noexcept {
    if (!journal_) return;
    auto& entry = entries_[index]; auto& data = entry.metadata; const auto attempt = data.attempt; TraceFact fact;
    fact.subject = {attempt.step.run,TraceNodeKind::tool,attempt.step.sequence,attempt.sequence,data.candidate}; fact.kind = kind;
    fact.result = entry.request ? entry.request->operation()->state() : data.result; fact.exit = entry.request ? entry.request->exit()->state() : data.exit;
    fact.effect = entry.request ? entry.request->outcome().effect : data.effect; fact.error = entry.request ? entry.request->outcome().error : data.error;
    fact.fault = entry.request ? entry.request->fault() : data.fault; fact.related = data.invocation;
    if (entry.last && entry.last->result == fact.result && entry.last->exit == fact.exit && entry.last->effect == fact.effect &&
        entry.last->error == fact.error && entry.last->fault == fact.fault && entry.last->related == fact.related) return;
    fact.source_revision = ++data.revision; journal_->append(fact); entry.last = fact;
}
// 投影已经裁定的控制决定，不调用规则、参数解析或插件函数。
void ToolBatch::publish_policy(std::size_t index, ControlPoint point, PolicyDecision decision) noexcept {
    if (!journal_) return;
    auto& data = entries_[index].metadata; const auto attempt = data.attempt; TraceFact fact;
    fact.subject = {attempt.step.run,TraceNodeKind::tool,attempt.step.sequence,attempt.sequence,data.candidate}; fact.kind = TraceFactKind::policy_decision;
    fact.control_point = point; fact.decision = decision; fact.source_revision = ++data.revision; journal_->append(fact);
}
// 为拒绝候选记录零动作，不改写已经消费或发生效果的确认。
void ToolBatch::reject(std::size_t index, ConfirmationState reason) {
    auto& entry = entries_[index];
    if (entry.metadata.confirmation) confirmations_->invalidate(entry.metadata.confirmation,reason);
    context_.set_tool_result(index,{false,ToolEffect::not_applied,{},ToolError::unavailable},runtime::OperationState::failed,ToolError::none,false);
    entry.metadata.result = runtime::OperationState::failed; entry.metadata.exit = runtime::OperationState::succeeded; entry.filled = true;
    publish(index);
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
            entry.metadata.tool_id = selected.description.id;
            auto candidate = current; candidate.tool_requests = bound_.tool_requests+index;
            const auto previous = entry.metadata.before;
            entry.metadata.before = restrict_policy(policy_.before_tool(candidate,selected),extension_,ControlPoint::tool_start,candidate);
            if (previous != entry.metadata.before) publish_policy(index,ControlPoint::tool_start,*entry.metadata.before);
            const auto decision = *entry.metadata.before;
            if (decision.verdict == PolicyVerdict::deny) { reject(index); continue; }
            if (decision.verdict == PolicyVerdict::wait) {
                if (!confirmations_ || decision.reason != PolicyReason::confirmation_required) { reject(index); continue; }
                if (!entry.metadata.confirmation) entry.metadata.confirmation = confirmations_->offer(confirmation_target(candidate,selected,entry.call,entry.metadata.candidate));
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
                confirmation_target(current,tools_.resolve(entry.call.name,entry.call.arguments),entry.call,entry.metadata.candidate),current.now)) { reject(index); continue; }
            entry.request = executor_.submit(current.attempt,entry.call); entry.metadata.accepted = true;
            entry.metadata.invocation = entry.request->invocation().invocation;
            publish(index);
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
        if (current.attempt != bound_.attempt || current.plan_revision != bound_.plan_revision || current.plan_step != bound_.plan_step ||
            current.observation_binding != bound_.observation_binding) decision = {PolicyVerdict::deny,PolicyReason::invalid_context};
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
            publish(index);
            if (entry.request->exit()->state() == OperationState::pending) { complete = false; continue; }
            entry.metadata.after = restrict_policy(policy_.after_tool(current,*entry.request),extension_,ControlPoint::tool_result,current);
            publish_policy(index,ControlPoint::tool_result,*entry.metadata.after);
            if (entry.metadata.after->verdict != PolicyVerdict::allow) {
                continuation_ = *entry.metadata.after; executor_.close(CancelReason::owner_stop);
            }
            context_.set_tool_result(index,entry.request->outcome(),entry.request->operation()->state(),entry.request->fault());
            entry.filled = true;
        }
        if (complete) {
            context_.commit_tools(); const auto& messages = *context_.committed();
            for (std::size_t index = messages.size()-entries_.size(); index < messages.size(); ++index)
                add_fingerprint(fingerprint_,messages[index].content);
            done_ = true;
        }
    } catch (...) { advancing_ = false; throw; }
    advancing_ = false; return done_;
}
// 只返回完成时固定的指纹，不重新解析参数、读取工具或依赖上下文继续存活。
std::uint64_t ToolBatch::progress_fingerprint() const {
    if (!done_) throw std::logic_error("batch: progress is not committed");
    return fingerprint_;
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
// 可信验证读取原工具回执，不从模型转述或未完成占位中推测实际结果。
std::optional<ToolResult> ToolBatch::result(runtime::DomainId candidate) const {
    for (const auto& entry : entries_) if (entry.metadata.candidate == candidate && entry.request &&
        entry.request->exit()->state() == runtime::OperationState::succeeded) return entry.request->outcome();
    return std::nullopt;
}
}
