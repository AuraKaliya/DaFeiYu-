#include <dafeiyu/owner_scheduler.hpp>
#include <algorithm>
#include <limits>

namespace dfy::runtime {
// 固定所有者目录及引用容量，失败不返回可使用的调度器。
OwnerScheduler::OwnerScheduler(std::size_t owners) : owners_(owners), slots_(owners) {}
// 完整创建队列后发布所有者，权重限制避免单次份额过大。
// 步骤：校验权重与容量 → 定位空名额 → 分配并签发独立身份 → 提交目录。
Handle OwnerScheduler::add_owner(std::size_t weight, std::size_t capacity) {
    if (!weight || weight > 64 || !capacity) throw std::invalid_argument("scheduler: weight 1..64 and positive capacity required");
    const auto free = std::find(slots_.begin(),slots_.end(),Handle{});
    if (free == slots_.end()) throw std::length_error("scheduler: owner capacity exhausted");
    const auto handle = owners_.insert(std::make_shared<OwnerReference>(OwnerReference{std::make_shared<Owner>(weight,capacity)}),false); *free = handle;
    return handle;
}
// 停止新步骤接纳，不抹除尚待运行或挂起的记录。
void OwnerScheduler::stop(Handle owner) { checked(owner).accepting = false; }
// 在全部工作退出后归还名额，不自动放弃仍有前置等待的任务。
void OwnerScheduler::remove(Handle owner) {
    const auto& record = checked(owner);
    if (record.accepting || record.used || record.running) throw std::logic_error("scheduler: owner not stopped and drained");
    const auto slot = std::find(slots_.begin(),slots_.end(),owner);
    if (static_cast<std::size_t>(slot - slots_.begin()) == cursor_) remaining_ = 0;
    owners_.release(owner); *slot = {};
}
// 预检全部条件后无失败点转交任务与前置持有，接纳失败不改变序号。
// 步骤：校验所有者和任务 → 检查在途容量及序号 → 保存任务 → 推进接纳序号。
void OwnerScheduler::post(Handle owner, FifoExecutor::Task task, std::shared_ptr<const Operation> prerequisite) {
    auto& record = checked(owner);
    if (!task) throw std::invalid_argument("scheduler: task required");
    if (!record.accepting) throw std::logic_error("scheduler: owner stopped");
    if (record.used == record.entries.size() || !next_order_) throw std::length_error("scheduler: task capacity exhausted");
    for (auto& slot : record.entries) if (!slot.task) {
        slot = Entry{std::move(task),std::move(prerequisite),next_order_}; ++record.used;
        next_order_ = next_order_ == std::numeric_limits<std::uint64_t>::max() ? 0 : next_order_ + 1;
        return;
    }
}
// 只考虑可执行集合，取消或失败的前置也允许任务运行自己的收尾逻辑。
std::optional<std::size_t> OwnerScheduler::eligible(const Owner& owner) const noexcept {
    std::optional<std::size_t> result;
    for (std::size_t index = 0; index < owner.entries.size(); ++index) {
        const auto& entry = owner.entries[index];
        if (!entry.task || (entry.prerequisite && entry.prerequisite->state() == OperationState::pending)) continue;
        if (!result || entry.order < owner.entries[*result].order) result = index;
    }
    return result;
}
// 每次调用只执行一个步骤，空闲所有者不积攒份额，执行和捕获资源析构都保留活动保护。
// 步骤：拒绝重入 → 有界寻找就绪所有者 → 扣除本轮权重份额 → 隔离执行 → 归还活动容量。
RunResult OwnerScheduler::run_one() {
    if (running_) throw std::logic_error("scheduler: recursive execution");
    for (std::size_t visited = 0; visited < slots_.size(); ++visited) {
        auto* owner = slots_[cursor_].empty() ? nullptr : &checked(slots_[cursor_]);
        const auto index = owner ? eligible(*owner) : std::nullopt;
        if (!index) { remaining_ = 0; cursor_ = (cursor_ + 1) % slots_.size(); continue; }
        if (!remaining_) remaining_ = owner->weight;
        if (!--remaining_) cursor_ = (cursor_ + 1) % slots_.size();

        auto entry = std::move(owner->entries[*index]); owner->entries[*index] = {};
        running_ = true; owner->running = true; auto result = RunResult::executed;
        try { entry.task(); } catch (...) { result = RunResult::failed; }
        entry = {}; --owner->used; owner->running = false; running_ = false;
        return result;
    }
    return RunResult::idle;
}
}
