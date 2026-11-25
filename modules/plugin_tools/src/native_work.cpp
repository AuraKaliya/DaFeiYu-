#include <dafeiyu/plugin_tool_work.hpp>
#include <dafeiyu/tool_abi_v2.h>

namespace dfy::plugins {
namespace {
using namespace agent;
// 借用宿主字符串给一次原生调用，不把借用保存到插件工作中。
dfy_v2_string_view view(const std::string& text) { return {text.data(),text.size()}; }
// 明确转换取消原因，不依赖 C++ 与 C 枚举的底层取值相同。
std::uint32_t native_cancel(runtime::CancelReason reason) {
    if (reason == runtime::CancelReason::user) return DFY_V2_CANCEL_USER;
    if (reason == runtime::CancelReason::deadline) return DFY_V2_CANCEL_DEADLINE;
    return DFY_V2_CANCEL_OWNER_STOP;
}
// 检查成功回执及错误缓冲约束，错误正文不进入诊断。
bool success(std::uint32_t status, const dfy_v2_error& error) { return status == DFY_V2_OK && error.length == 0; }
// 在工作存在期间保留实例和能力，释放成功之前不向驱动报告真实退出。
class NativeWork final : public ToolWork {
  public:
    // 完整准备宿主对象与代码持有，尚未调用 begin。
    NativeWork(std::shared_ptr<PluginInstance> instance, InstanceCapability<dfy_v2_tool_api> capability,
        std::string id, std::uint32_t version)
        : instance_(std::move(instance)), capability_(std::move(capability)), id_(std::move(id)), version_(version) {}
    // 提交一次原生工作，异常或畸形接纳不能丢弃可能存在的工作责任。
    // 步骤：构造借用调用 → 在实例保护内接纳 → 按票据和回执区分拒绝、有效工作或隔离保留。
    void begin(const ToolInvocation& input) noexcept {
        const dfy_v2_tool_call call{sizeof(dfy_v2_tool_call),version_,input.attempt.step.run.session,input.attempt.step.run.sequence,
            input.attempt.step.sequence,input.attempt.sequence,input.invocation,view(input.call.id),view(input.call.arguments)};
        try {
            dfy_v2_error error{};
            const auto status = instance_->invoke(capability_,[&](const auto& api,auto* native) { return api.begin(native,view(id_),&call,&ticket_,&error); });
            const bool complete = ticket_.owner && ticket_.sequence;
            if (!success(status,error)) {
                if (status && !ticket_.owner && !ticket_.sequence && error.length <= DFY_V2_ERROR_CAPACITY) rejected_ = true;
                else if (complete) broken_begin_ = true;
                else quarantined_ = true;
            } else if (!complete) quarantined_ = true;
        } catch (...) { if (ticket_.owner && ticket_.sequence) broken_begin_ = true; else quarantined_ = true; }
    }
    // 非阻塞推进原生工作，复制借用结果后才调用释放；清理故障与业务结果分别返回。
    // 步骤：处理拒绝或隔离状态 → 提交取消 → 读取并校验结果 → 尝试释放已退出工作 → 返回独立故障。
    ToolProgress poll(runtime::CancelReason cancellation) override {
        if (rejected_) return {ToolResult{false,ToolEffect::not_applied,{},ToolError::failed},true};
        if (quarantined_) return {{},false,ToolError::invalid_result};
        if (done_) return {{},true};
        if (broken_begin_ && cancellation == runtime::CancelReason::none) cancellation = runtime::CancelReason::owner_stop;
        return instance_->invoke(capability_,
        // 在同一次实例保护内推进与复制结果，释放失败仍保持票据和能力。
        // 步骤：发送取消 → 读取并转换结果 → 释放已退出工作 → 归还故障与退出状态。
        [&](const auto& api,auto* native) {
            ToolProgress result; dfy_v2_error error{};
            if (!native_exited_ && cancellation != runtime::CancelReason::none && !cancel_sent_) {
                if (success(api.request_cancel(native,ticket_,native_cancel(cancellation),&error),error)) cancel_sent_ = true;
                else result.fault = ToolError::cancel_failed;
            }
            if (!native_exited_) {
                dfy_v2_tool_progress progress{}; progress.struct_size = sizeof(progress); error = {};
                if (!success(api.poll(native,ticket_,&progress,&error),error)) return ToolProgress{{},false,ToolError::read_failed};
                if (progress.struct_size != sizeof(progress) || progress.reserved || progress.result_ready > 1 || progress.exited > 1 ||
                    (emitted_ && !progress.result_ready) || (progress.result_ready && (progress.succeeded > 1 ||
                    progress.effect < DFY_V2_TOOL_NOT_APPLIED || progress.effect > DFY_V2_TOOL_UNKNOWN)))
                    return ToolProgress{{},false,ToolError::invalid_result};
                native_exited_ = progress.exited != 0;
                if (progress.result_ready && !emitted_) {
                    ToolResult output; output.succeeded = progress.succeeded != 0;
                    output.effect = progress.effect == DFY_V2_TOOL_APPLIED ? ToolEffect::applied :
                        progress.effect == DFY_V2_TOOL_NOT_APPLIED ? ToolEffect::not_applied : ToolEffect::unknown;
                    output.error = output.succeeded ? ToolError::none : ToolError::failed;
                    if (progress.output.length > 64*1024 || (progress.output.length && !progress.output.data)) {
                        output.succeeded = false; output.error = ToolError::invalid_result; result.fault = ToolError::invalid_result;
                    } else {
                        try { if (progress.output.length) output.output.assign(progress.output.data,static_cast<std::size_t>(progress.output.length)); }
                        catch (...) { output.succeeded = false; output.error = ToolError::invalid_result; result.fault = ToolError::invalid_result; }
                    }
                    if (broken_begin_) { output.succeeded = false; output.error = ToolError::invalid_result; result.fault = ToolError::invalid_result; }
                    result.result = std::move(output); emitted_ = true;
                }
            }
            if (native_exited_) {
                error = {};
                if (success(api.release(native,ticket_,&error),error)) { ticket_ = {}; done_ = true; result.exited = true; }
                else result.fault = ToolError::release_failed;
            }
            return result;
        });
    }
  private:
    std::shared_ptr<PluginInstance> instance_;
    InstanceCapability<dfy_v2_tool_api> capability_;
    std::string id_;
    std::uint32_t version_;
    dfy_v2_tool_ticket ticket_{};
    bool rejected_ = false, quarantined_ = false, broken_begin_ = false;
    bool emitted_ = false, native_exited_ = false, cancel_sent_ = false, done_ = false;
};
}
// 创建只引用原实例的宿主工厂，空闲目录不持有原生代码，实际工作独立保持代码寿命。
// 步骤：校验描述标识 → 捕获弱实例 → 实际调用时核对实例与能力 → 先准备对象再接纳原生工作。
agent::ToolFactory native_tool_factory(std::weak_ptr<PluginInstance> instance, std::string local_id, std::uint32_t version) {
    if (local_id.empty() || local_id.size() > 32 || !version) throw std::invalid_argument("native tool: invalid identity");
    for (const auto c : local_id) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-')) throw std::invalid_argument("native tool: invalid local name");
    // 从原弱实例准备一次工作，准备失败不调用原生 begin，接纳后由工作独立持有代码。
    // 步骤：核对实例 → 查询并核对能力 → 完整创建宿主工作对象 → 提交一次原生工作。
    return [instance = std::move(instance),id = std::move(local_id),version](const ToolInvocation& input) -> std::unique_ptr<ToolWork> {
        const auto owner = instance.lock();
        if (!owner || owner->phase() != InstancePhase::active) return completed_tool({false,ToolEffect::not_applied,{},ToolError::unavailable});
        auto capability = owner->query<dfy_v2_tool_api>(DFY_V2_TOOL_INTERFACE,DFY_V2_TOOL_VERSION);
        if (!capability || !owner->invoke(*capability,[](const auto& api,auto*) {
            return api.count && api.describe && api.begin && api.poll && api.request_cancel && api.release;
        })) return completed_tool({false,ToolEffect::not_applied,{},ToolError::unavailable});
        auto work = std::make_unique<NativeWork>(owner,std::move(*capability),id,version);
        work->begin(input); return work;
    };
}
}
