#include <dafeiyu/tool_work.hpp>
#include <dafeiyu/utf8.hpp>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace dfy::agent {
// 将宿主最后一次前置检查转换为可沿原调用核对的有界事实。
std::string ToolStartDecision::detail(std::string_view stage) const {
    const auto reason = block == ToolStartBlock::user_input_pending ? "user_input_pending" :
        block == ToolStartBlock::observation_stale ? "observation_stale" : "conditions_unavailable";
    nlohmann::json data{{"stage",stage},{"reason",reason}};
    if (observation) data["observationBinding"] = observation;
    return data.dump();
}
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
    ToolDriver::CheckedFactory factory;
    std::unique_ptr<ToolWork> work;
    std::shared_ptr<runtime::SerialDomain> domain;
    std::shared_ptr<const runtime::SerialRequest> ordering;
    std::optional<ToolTime> deadline;
    ToolClock now;
    ToolStartGuard before_start;
    ToolWorkState* next = nullptr;
    bool started = false, advancing = false, reported = false;
    // 完整准备记录和调用对象，计入进程内最多 256 个活动或保留驱动状态。
    // 步骤：创建请求与资源责任 → 绑定原顺序资格 → 登记驱动计数。
    ToolWorkState(ToolInvocation invocation, ToolDriver::CheckedFactory callback, ToolResources resources)
        : record(new ToolRequest(std::move(invocation),std::move(resources.charge))), factory(std::move(callback)),
          domain(std::move(resources.domain)), ordering(std::move(resources.ordering)),
          deadline(resources.deadline), now(std::move(resources.now)),before_start(std::move(resources.before_start)) {
        if (domain) domain->bind(ordering);
        ++resident_count;
    }
    // 已确认退出后释放状态计数，工作必须先在推进阶段销毁。
    ~ToolWorkState() { --resident_count; }
    // 始终保留宿主故障，仅在业务尚未结算时报告失败，不改写效果或取消终态。
    void fail(ToolError error) noexcept {
        record->fault_ = error;
        if (record->exit()->state() == runtime::OperationState::pending) record->request_cancel(runtime::CancelReason::owner_stop);
        if (record->result_.operation()->state() == runtime::OperationState::pending) {
            record->outcome_.error = error; record->result_.settle(runtime::OperationState::failed);
        }
    }
    // 确认工作退出并归还原顺序占用，保留结果的预算不随工作退出提前归还。
    // 步骤：销毁工作和工厂 → 完成占用或撤回未开始资格 → 发布退出确认。
    void finish() {
        using namespace runtime;
        work.reset(); factory = {}; before_start = {};
        if (domain) {
            if (started) domain->finish(ordering);
            else if (ordering->phase() != SerialPhase::cancelled && ordering->withdrawal_reason() == CancelReason::none)
                domain->withdraw(ordering,record->exit()->cancellation_reason());
        }
        record->exit_.settle(OperationState::succeeded);
    }
    // 在回调前后裁定取消和期限，时钟故障停止业务但仍等待工作实际退出。
    // 步骤：保留已有取消首因 → 核对截止时刻或时钟错误 → 裁定尚未结算的业务取消。
    runtime::CancelReason cancel() {
        using namespace runtime;
        if (record->exit()->cancellation_reason() == CancelReason::none && deadline) {
            try { if (now() >= *deadline) record->request_cancel(CancelReason::deadline); }
            catch (...) { fail(ToolError::invalid_result); record->request_cancel(CancelReason::owner_stop); }
        }
        const auto reason = record->exit()->cancellation_reason();
        if (reason != CancelReason::none && record->operation()->state() == OperationState::pending) {
            record->outcome_.error = reason == CancelReason::deadline ? ToolError::deadline : ToolError::cancelled;
            record->result_.settle(OperationState::cancelled);
        }
        return reason;
    }
    // 推进真实工作与唯一结果，取消只停止后续接纳，迟到效果仍然记录。
    // 步骤：核对顺序、前置及取消 → 首次提交工厂 → 读取并校验工作报告 → 结算业务 → 按实际退出回收。
    void advance() {
        using namespace runtime;
        if (advancing) throw std::logic_error("tool work: reentrant advance");
        if (record->exit()->state() != OperationState::pending) return;
        advancing = true;
        if (!started && domain) {
            auto reason = ordering->withdrawal_reason();
            if (reason == CancelReason::none && ordering->phase() == SerialPhase::cancelled) reason = ordering->admission()->cancellation_reason();
            if (reason != CancelReason::none) record->request_cancel(reason);
        }
        const auto cancellation = cancel();
        if (!started && cancellation != CancelReason::none) { finish(); advancing = false; return; }
        if (!started) {
            if (domain && ordering->phase() == SerialPhase::waiting) { advancing = false; return; }
            if (before_start) {
                ToolStartDecision decision(false);
                try { decision = before_start(); } catch (...) {}
                if (cancel() != CancelReason::none) { finish(); advancing = false; return; }
                if (decision.block != ToolStartBlock::none) {
                    record->outcome_.output = decision.detail("before_start");
                    fail(ToolError::unavailable); finish(); advancing = false; return;
                }
            }
            if (domain) {
                try { domain->begin(ordering); } catch (...) { advancing = false; throw; }
            }
            started = true; record->outcome_.effect = ToolEffect::unknown;
            try { work = factory(record->invocation_,record->dispatched_); }
            catch (...) { cancel(); fail(ToolError::failed); finish(); advancing = false; return; }
            if (!work) { cancel(); fail(ToolError::invalid_result); finish(); advancing = false; return; }
        }

        try {
            auto progress = work->poll(cancel()); cancel();
            if (progress.result && !reported) {
                auto& result = *progress.result;
                const bool effect_valid = result.effect == ToolEffect::not_applied || result.effect == ToolEffect::applied || result.effect == ToolEffect::unknown;
                if (effect_valid) record->outcome_.effect = result.effect;
                const bool output_valid = result.output.size() <= 64*1024 && runtime::valid_utf8(result.output);
                if (!output_valid || !effect_valid ||
                    static_cast<unsigned>(result.error) > static_cast<unsigned>(ToolError::deadline) ||
                    (result.succeeded != (result.error == ToolError::none))) {
                    fail(ToolError::invalid_result);
                } else {
                    record->outcome_ = std::move(result); reported = true;
                    if (record->operation()->state() == OperationState::pending)
                        record->result_.settle(record->outcome_.succeeded ? OperationState::succeeded : OperationState::failed);
                }
            }
            if (progress.fault != ToolError::none) {
                fail(static_cast<unsigned>(progress.fault) <= static_cast<unsigned>(ToolError::deadline) ? progress.fault : ToolError::invalid_result);
            }
            if (progress.exited) {
                if (!reported) fail(ToolError::invalid_result);
                finish();
            }
        } catch (...) { cancel(); fail(ToolError::invalid_result); }
        advancing = false;
    }
};
// 直接工厂已确定实现，实际调用前固定一次派发事实。
ToolDriver::ToolDriver(ToolInvocation invocation, ToolFactory factory, ToolResources resources)
    : ToolDriver(factory ? CheckedFactory([implementation=std::move(factory)](const ToolInvocation& input,bool& dispatched) {
        dispatched = true;
        return implementation(input);
    }) : CheckedFactory{},std::move(invocation),std::move(resources)) {}
// 在任何工具代码执行之前验证调用身份、工厂和有界驱动容量。
// 步骤：校验调用及资源归属 → 检查驱动容量 → 准备状态并绑定顺序责任。
ToolDriver::ToolDriver(CheckedFactory factory, ToolInvocation invocation, ToolResources resources) {
    if (!factory || !invocation.invocation || !invocation.attempt.step.run.session || !invocation.attempt.step.run.sequence ||
        !invocation.attempt.step.sequence || !invocation.attempt.sequence || invocation.call.id.empty() || invocation.call.name.empty())
        throw std::invalid_argument("tool work: identity and factory required");
    if (static_cast<bool>(resources.domain) != static_cast<bool>(resources.ordering) ||
        (resources.domain && resources.ordering->domain() != resources.domain->id()))
        throw std::invalid_argument("tool work: matching ordering resources required");
    if (resources.deadline && !resources.now) throw std::invalid_argument("tool work: deadline clock required");
    if (resident_count >= 256) throw std::length_error("tool work: driver capacity exhausted");
    state_ = std::make_unique<ToolWorkState>(std::move(invocation),std::move(factory),std::move(resources));
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
