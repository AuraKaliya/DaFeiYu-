#include <dafeiyu/tool_work.hpp>
#include <stdexcept>

namespace dfy::agent {
namespace {
class CompletedWork final : public ToolWork {
  public:
    // 接收已经发生的结果，后续只转交结果和退出事实。
    explicit CompletedWork(ToolResult result) : result_(std::move(result)) {}
    // 返回已有结果并确认没有剩余工作，不产生新的副作用。
    ToolProgress poll(runtime::CancelReason) override { return {std::move(result_),true}; }
  private:
    ToolResult result_;
};
ToolWorkState* retained_head = nullptr;
std::size_t resident_count = 0, retained_count = 0;
bool reaping = false;
}
// 包装同步结果以复用统一工作生命周期，不重新执行原动作。
std::unique_ptr<ToolWork> completed_tool(ToolResult result) { return std::make_unique<CompletedWork>(std::move(result)); }
struct ToolWorkState {
    std::shared_ptr<ToolRequest> record;
    ToolFactory factory;
    std::unique_ptr<ToolWork> work;
    ToolWorkState* next = nullptr;
    bool started = false, advancing = false, reported = false;
    // 完整准备记录和调用对象，计入进程内最多 256 个活动或保留驱动状态。
    ToolWorkState(ToolInvocation invocation, ToolFactory callback)
        : record(new ToolRequest(std::move(invocation))), factory(std::move(callback)) { ++resident_count; }
    // 已确认退出后释放状态计数，工作必须先在推进阶段销毁。
    ~ToolWorkState() { --resident_count; }
    // 只在业务尚未结算时报告失败，不改写已发生效果或取消终态。
    void fail(ToolError error) noexcept {
        if (record->result_.operation()->state() == runtime::OperationState::pending) {
            record->outcome_.error = error; record->result_.settle(runtime::OperationState::failed);
        }
    }
    // 确认当前工作已不再执行，销毁工作后才发布退出。
    void finish() {
        work.reset(); factory = {}; record->exit_.settle(runtime::OperationState::succeeded);
    }
    // 在接收回调结果前重新裁定取消，回调内请求取消也优先于迟到成功。
    runtime::CancelReason cancel() {
        using namespace runtime;
        const auto reason = record->exit()->cancellation_reason();
        if (reason != CancelReason::none && record->operation()->state() == OperationState::pending) {
            record->outcome_.error = reason == CancelReason::deadline ? ToolError::deadline : ToolError::cancelled;
            record->result_.settle(OperationState::cancelled);
        }
        return reason;
    }
    // 推进真实工作与唯一结果，取消只停止后续接纳，迟到效果仍然记录。
    // 步骤：裁定取消 → 首次提交工厂 → 读取并校验工作报告 → 结算业务 → 按实际退出回收。
    void advance() {
        using namespace runtime;
        if (advancing) throw std::logic_error("tool work: reentrant advance");
        if (record->exit()->state() != OperationState::pending) return;
        advancing = true;
        const auto cancellation = cancel();
        if (!started && cancellation != CancelReason::none) { finish(); advancing = false; return; }
        if (!started) {
            started = true; record->outcome_.effect = ToolEffect::unknown;
            try { work = factory(record->invocation_); }
            catch (...) { cancel(); fail(ToolError::failed); finish(); advancing = false; return; }
            if (!work) { cancel(); fail(ToolError::invalid_result); finish(); advancing = false; return; }
        }

        try {
            auto progress = work->poll(cancel()); cancel();
            if (progress.result && !reported) {
                auto& result = *progress.result;
                const bool effect_valid = result.effect == ToolEffect::not_applied || result.effect == ToolEffect::applied || result.effect == ToolEffect::unknown;
                if (result.output.size() > 64*1024 || !effect_valid ||
                    static_cast<unsigned>(result.error) > static_cast<unsigned>(ToolError::deadline) ||
                    (result.succeeded != (result.error == ToolError::none))) {
                    if (effect_valid) record->outcome_.effect = result.effect;
                    fail(ToolError::invalid_result);
                } else {
                    record->outcome_ = std::move(result); reported = true;
                    if (record->operation()->state() == OperationState::pending)
                        record->result_.settle(record->outcome_.succeeded ? OperationState::succeeded : OperationState::failed);
                }
            }
            if (progress.exited) {
                if (!reported) fail(ToolError::invalid_result);
                finish();
            }
        } catch (...) { cancel(); fail(ToolError::invalid_result); }
        advancing = false;
    }
};
// 在任何工具代码执行之前验证调用身份、工厂和有界驱动容量。
ToolDriver::ToolDriver(ToolInvocation invocation, ToolFactory factory) {
    if (!factory || !invocation.invocation || !invocation.attempt.step.run.session || !invocation.attempt.step.run.sequence ||
        !invocation.attempt.step.sequence || !invocation.attempt.sequence || invocation.call.id.empty() || invocation.call.name.empty())
        throw std::invalid_argument("tool work: identity and factory required");
    if (resident_count >= 256) throw std::length_error("tool work: driver capacity exhausted");
    state_ = std::make_unique<ToolWorkState>(std::move(invocation),std::move(factory));
}
// 关闭局部驱动时保持未退出责任，不以析构或业务取消冒充原生代码退出。
// 步骤：请求所有者停止 → 尝试推进一次 → 已退出释放，否则转入预分配保留链。
ToolDriver::~ToolDriver() {
    state_->record->request_cancel(runtime::CancelReason::owner_stop);
    try { state_->advance(); } catch (...) {}
    if (state_->record->exit()->state() == runtime::OperationState::pending) {
        auto* state = state_.release(); state->next = retained_head; retained_head = state; ++retained_count;
    }
}
// 返回独立请求持有，实际工作责任始终留在驱动或保留表中。
std::shared_ptr<ToolRequest> ToolDriver::request() const noexcept { return state_->record; }
// 推进本驱动的一次工作阶段，不调度其他工具。
void ToolDriver::advance() { state_->advance(); }
// 返回保留链中的未退出驱动数量。
std::size_t ToolDriver::retained() noexcept { return retained_count; }
// 重试孤立工作收尾，拒绝回调重入造成保留链重复处理。
// 步骤：占用重试入口 → 逐项推进并删除已退出状态 → 解除入口占用并返回余量。
std::size_t ToolDriver::retry_retained() {
    if (reaping) throw std::logic_error("tool work: reentrant retained cleanup");
    reaping = true; auto* remaining = std::exchange(retained_head,nullptr);
    while (remaining) {
        auto* state = remaining; remaining = state->next;
        try { state->advance(); } catch (...) {}
        if (state->record->exit()->state() == runtime::OperationState::pending) { state->next = retained_head; retained_head = state; }
        else { delete state; --retained_count; }
    }
    reaping = false; return retained_count;
}
}
