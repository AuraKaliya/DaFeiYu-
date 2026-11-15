#include <dafeiyu/run_context.hpp>
#include <dafeiyu/utf8.hpp>
#include <algorithm>
#include <stdexcept>

namespace dfy::agent {
namespace {
// 将实际操作终态转换为模型可读标签，不推断目标已完成。
const char* state_name(runtime::OperationState state) {
    if (state == runtime::OperationState::succeeded) return "succeeded";
    if (state == runtime::OperationState::failed) return "failed";
    if (state == runtime::OperationState::cancelled) return "cancelled";
    throw std::invalid_argument("context: terminal tool result required");
}
// 显式表达动作效果，不把取消等同于未发生。
const char* effect_name(ToolEffect effect) {
    if (effect == ToolEffect::not_applied) return "not_applied";
    if (effect == ToolEffect::applied) return "applied";
    if (effect == ToolEffect::unknown) return "unknown";
    throw std::invalid_argument("context: invalid effect");
}
// 返回固定错误标签，避免把异常正文或凭据带入上下文。
const char* error_name(ToolError error) {
    switch (error) {
    case ToolError::none: return "none";
    case ToolError::failed: return "failed";
    case ToolError::unavailable: return "unavailable";
    case ToolError::invalid_result: return "invalid_result";
    case ToolError::cancel_failed: return "cancel_failed";
    case ToolError::read_failed: return "read_failed";
    case ToolError::release_failed: return "release_failed";
    case ToolError::cancelled: return "cancelled";
    case ToolError::deadline: return "deadline";
    }
    throw std::invalid_argument("context: invalid tool error");
}
}
// 准备本轮用户消息并验证历史完整性，输入只在后续明确提交时进入历史。
// 步骤：保留旧快照 → 复制并追加本轮输入 → 校验角色、配对和容量。
RunContext::RunContext(std::shared_ptr<const MessageHistory> history, std::string prompt) : committed_(std::move(history)) {
    if (!committed_) throw std::invalid_argument("context: history required");
    pending_ = std::make_shared<MessageHistory>(*committed_); pending_->push_back({model::Role::user,std::move(prompt)});
    model::validate_request({*pending_});
}
// 未完成工具批次不能作为真实模型输入，避免占位文本冒充工具结果。
const MessageHistory& RunContext::messages() const {
    if (!replies_.empty()) throw std::logic_error("context: tool results not committed");
    return pending_ ? *pending_ : *committed_;
}
// 在所有动作之前完成结果容器和正文上限预留，提交中途不改变原上下文。
// 步骤：检查响应及事务阶段 → 复制助手调用并预留完整结果 → 校验整体预算 → 发布待填批次。
void RunContext::prepare_tools(const model::Message& assistant, const std::vector<model::ToolDefinition>& definitions) {
    if (finished_ || !replies_.empty()) throw std::logic_error("context: invalid tool preparation phase");
    model::validate_response({assistant,model::FinishReason::tool_calls});
    model::Request prepared{messages(),definitions}; prepared.messages.push_back(assistant);
    const auto start = prepared.messages.size(); std::vector<bool> replies(assistant.tool_calls.size(),false);
    for (const auto& call : assistant.tool_calls)
        prepared.messages.push_back({model::Role::tool,std::string(maximum_reply_bytes,' '),{},call.id});
    model::validate_request(prepared);
    auto pending = std::make_shared<MessageHistory>(std::move(prepared.messages));
    pending_ = std::move(pending); tool_begin_ = start; replies_.swap(replies);
}
// 在预留字符串内写入终态、效果和输出，已执行动作之后不为投影正文重新分配。
// 步骤：核对位置及终态 → 校验有界文本与标签 → 原位填写固定头和输出 → 标记该结果完成。
void RunContext::set_tool_result(std::size_t index, const ToolResult& outcome, runtime::OperationState state, ToolError fault, bool accepted) {
    if (finished_ || index >= replies_.size() || replies_[index]) throw std::logic_error("context: invalid or duplicate tool result");
    if (outcome.output.size() > 64*1024 || !runtime::valid_utf8(outcome.output) ||
        (!accepted && (state != runtime::OperationState::failed || outcome.effect != ToolEffect::not_applied)))
        throw std::invalid_argument("context: invalid tool outcome");
    const auto* status = state_name(state); const auto* effect = effect_name(outcome.effect);
    const auto* error = error_name(outcome.error); const auto* failure = error_name(fault);
    auto& output = (*pending_)[tool_begin_+index].content;
    if (output.capacity() < maximum_reply_bytes) throw std::logic_error("context: result reservation lost");
    output.clear(); output.append("status=").append(accepted ? status : "rejected");
    output.append("; effect=").append(effect).append("; error=").append(error).append("; fault=").append(failure);
    output.append("\noutput:\n").append(outcome.output); replies_[index] = true;
}
// 完整批次以共享不可变快照发布，不复制正文或依赖观察通知。
void RunContext::commit_tools() {
    if (replies_.empty() || std::find(replies_.begin(),replies_.end(),false) != replies_.end())
        throw std::logic_error("context: incomplete tool batch");
    committed_ = pending_; pending_.reset(); replies_.clear();
}
// 最终回答独立准备和提交，不回滚已经发布的工具调用及效果。
// 步骤：核对事务阶段 → 复制完整输入并追加回答 → 校验预算 → 发布最终历史。
void RunContext::finish_text(std::string answer) {
    if (finished_ || !replies_.empty()) throw std::logic_error("context: invalid final text phase");
    model::Request prepared{messages()}; prepared.messages.push_back({model::Role::assistant,std::move(answer)});
    model::validate_request(prepared);
    auto committed = std::make_shared<const MessageHistory>(std::move(prepared.messages));
    committed_ = std::move(committed); pending_.reset(); finished_ = true;
}
// 封闭不含待提交工具的事务，取消或失败不会抹去先前工具结果。
void RunContext::seal() {
    if (!replies_.empty()) throw std::logic_error("context: pending tool facts cannot be discarded");
    pending_.reset(); finished_ = true;
}
}
