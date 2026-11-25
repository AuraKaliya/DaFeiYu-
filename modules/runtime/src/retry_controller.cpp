#include <dafeiyu/retry_controller.hpp>
#include <algorithm>
#include <stdexcept>

namespace dfy::runtime {
// 在接纳前准备所有就绪状态和尝试名额，避免失败后重调度依赖新的内存分配。
// 步骤：校验次数、退避和期限 → 预留元数据预算 → 完整创建尝试槽 → 发布首项就绪并检查截止。
RetryController::RetryController(RetryPolicy policy, RetryTime accepted, RetryTime deadline, std::shared_ptr<Budget> budget)
    : domain_(next_domain()), policy_(policy), deadline_(deadline), last_(accepted), due_(accepted), delay_(policy.initial_delay) {
    if (!policy.maximum_attempts || policy.maximum_attempts > 64 || policy.initial_delay.count() <= 0 ||
        policy.maximum_delay < policy.initial_delay || policy.maximum_delay > std::chrono::hours(24) || deadline < accepted)
        throw std::invalid_argument("retry: invalid bounded policy");
    if (budget) charge_ = budget->acquire({policy.maximum_attempts,0});
    entries_.reserve(policy.maximum_attempts);
    for (std::size_t index = 0; index < policy.maximum_attempts; ++index) entries_.push_back(std::make_unique<Attempt>());
    entries_.front()->ready.settle(OperationState::succeeded); advance(accepted);
}
// 只结束尚未运行的宿主等待，不把控制器释放当作原生尝试实际退出。
RetryController::~RetryController() {
    if (phase_ == RetryPhase::running || result_.operation()->state() != OperationState::pending) return;
    result_.request_cancel(CancelReason::owner_stop); result_.settle(OperationState::cancelled); finish_waiting();
}
// 对已暴露的退避等待发布取消，首项已经就绪时保持该授予事实。
void RetryController::finish_waiting() {
    if (started_ < entries_.size() && entries_[started_]->ready.operation()->state() == OperationState::pending)
        entries_[started_]->ready.settle(OperationState::cancelled);
    phase_ = RetryPhase::finished;
}
// 期限和协作取消先于新一次尝试，退避到期只提供就绪资格。
// 步骤：拒绝时钟倒退 → 裁定取消或截止 → 保留运行尝试或结束等待 → 按时发布退避就绪。
void RetryController::advance(RetryTime now) {
    if (now < last_) throw std::invalid_argument("retry: time moved backwards");
    last_ = now;
    if (operation()->state() != OperationState::pending) return;
    if (operation()->cancellation_reason() != CancelReason::none || now >= deadline_) {
        if (operation()->cancellation_reason() == CancelReason::none) result_.request_cancel(CancelReason::deadline);
        result_.settle(OperationState::cancelled);
        if (phase_ != RetryPhase::running) finish_waiting();
        return;
    }
    if (phase_ == RetryPhase::backoff && now >= due_) {
        entries_[started_]->ready.settle(OperationState::succeeded); phase_ = RetryPhase::ready;
    }
}
// 下一尝试的就绪视图可被调度器持有，等待取消也能使调度包装醒来收尾。
std::shared_ptr<const Operation> RetryController::readiness() const noexcept {
    return phase_ == RetryPhase::ready || phase_ == RetryPhase::backoff ? entries_[started_]->ready.operation() : nullptr;
}
// 消费一次调度机会，等待和过期不消耗尝试编号。
std::optional<AttemptId> RetryController::begin_attempt(RetryTime now) {
    advance(now);
    if (phase_ != RetryPhase::ready || operation()->state() != OperationState::pending) return {};
    const AttemptId id{domain_,started_ + 1};
    entries_[started_]->snapshot = {id,OperationState::pending,now,{}};
    ++started_; phase_ = RetryPhase::running; return id;
}
// 已退出尝试只结算一次，明确安全的失败才保留业务等待并重新经过调度。
// 步骤：核对当前身份及报告 → 裁定期限并记录真实退出 → 保留已定结果或完成业务 → 安排有界退避。
void RetryController::finish_attempt(AttemptId id, OperationState terminal, bool safe_to_retry, RetryTime now) {
    if (phase_ != RetryPhase::running || entries_[started_-1]->snapshot.id != id) throw std::logic_error("retry: attempt not active");
    if ((terminal != OperationState::succeeded && terminal != OperationState::failed && terminal != OperationState::cancelled) ||
        (safe_to_retry && terminal != OperationState::failed)) throw std::invalid_argument("retry: invalid attempt result");
    advance(now);
    auto& snapshot = entries_[started_-1]->snapshot; snapshot.state = terminal; snapshot.exited = now;
    if (operation()->state() != OperationState::pending) { phase_ = RetryPhase::finished; return; }
    if (terminal != OperationState::failed || !safe_to_retry || started_ == entries_.size()) {
        result_.settle(terminal); phase_ = RetryPhase::finished; return;
    }

    const auto interval = std::chrono::duration_cast<RetryTime::duration>(delay_);
    due_ = now > RetryTime::max() - interval ? RetryTime::max() : now + interval;
    due_ = std::min(due_,deadline_); phase_ = RetryPhase::backoff;
    delay_ = std::min(policy_.maximum_delay,delay_ * 2);
}
// 只返回已经实际开始的尝试，状态是副本，不推进时间或取消。
AttemptSnapshot RetryController::attempt(std::size_t index) const {
    if (index >= started_) throw std::out_of_range("retry: attempt not started");
    return entries_[index]->snapshot;
}
}
