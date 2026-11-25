#include <dafeiyu/owner_scheduler.hpp>
#include <algorithm>
#include <limits>

namespace dfy::runtime {
// 固定所有者目录及引用容量，失败不返回可使用的调度器。
OwnerScheduler::OwnerScheduler(std::size_t owners) : owners_(owners), slots_(owners) {}
// 完整创建队列后发布所有者，权重限制避免单次份额过大。
// 步骤：校验权重与容量 → 定位空名额 → 分配并签发独立身份 → 提交目录。
Handle OwnerScheduler::add_owner(std::size_t weight, std::size_t capacity, std::size_t control_reserve) {
    if (!weight || weight > 64 || !capacity || control_reserve > std::numeric_limits<std::size_t>::max() - capacity)
        throw std::invalid_argument("scheduler: valid weight and capacities required");
    const auto free = std::find(slots_.begin(),slots_.end(),Handle{});
    if (free == slots_.end()) throw std::length_error("scheduler: owner capacity exhausted");
    const auto handle = owners_.insert(std::make_shared<OwnerReference>(OwnerReference{std::make_shared<Owner>(weight,capacity,control_reserve)}),false); *free = handle;
    return handle;
}
// 停止新步骤接纳，不抹除尚待运行或挂起的记录。
void OwnerScheduler::stop(Handle owner) { checked(owner).accepting = false; }
// 在全部工作退出后归还名额，不自动放弃仍有前置等待的任务。
// 步骤：核对停止及排空 → 清除当前通路份额 → 释放所有者身份及目录槽。
void OwnerScheduler::remove(Handle owner) {
    const auto& record = checked(owner);
    if (record.accepting || record.used || record.running) throw std::logic_error("scheduler: owner not stopped and drained");
    const auto slot = std::find(slots_.begin(),slots_.end(),owner);
    for (std::size_t lane = 0; lane < 2; ++lane) if (static_cast<std::size_t>(slot - slots_.begin()) == cursor_[lane]) remaining_[lane] = 0;
    owners_.release(owner); *slot = {};
}
// 预检全部条件后无失败点转交任务与前置持有，接纳失败不改变序号。
// 步骤：校验所有者和任务 → 检查在途容量及序号 → 保存任务 → 推进接纳序号。
void OwnerScheduler::post(Handle owner, FifoExecutor::Task task, std::shared_ptr<const Operation> prerequisite, TaskPriority priority) {
    auto& record = checked(owner);
    if (!task) throw std::invalid_argument("scheduler: task required");
    if (!record.accepting) throw std::logic_error("scheduler: owner stopped");
    if (!available(owner,priority) || !next_order_) throw std::length_error("scheduler: task capacity exhausted");
    for (auto& slot : record.entries) if (!slot.task) {
        slot = Entry{std::move(task),std::move(prerequisite),next_order_,priority}; ++record.used;
        if (priority == TaskPriority::control) ++record.control_used;
        next_order_ = next_order_ == std::numeric_limits<std::uint64_t>::max() ? 0 : next_order_ + 1;
        return;
    }
}
// 分别核对普通和控制名额，非法优先级不借用任何通路。
std::size_t OwnerScheduler::available(Handle owner, TaskPriority priority) const {
    if (priority != TaskPriority::high && priority != TaskPriority::normal && priority != TaskPriority::low && priority != TaskPriority::control)
        throw std::invalid_argument("scheduler: invalid priority");
    const auto& record = checked(owner);
    if (!record.accepting) return 0;
    return priority == TaskPriority::control ? record.control_capacity - record.control_used : record.ordinary_capacity - (record.used - record.control_used);
}
// 只在可执行集合内分配优先级份额，未就绪高优先级不能阻挡有资格的低优先级。
// 步骤：选择控制通路或普通份额 → 查找该级最早就绪任务 → 返回记录及下一份额位置。
std::optional<OwnerScheduler::Choice> OwnerScheduler::eligible(const Owner& owner, bool control) const noexcept {
    constexpr std::array priorities{TaskPriority::high,TaskPriority::high,TaskPriority::high,TaskPriority::high,
        TaskPriority::normal,TaskPriority::normal,TaskPriority::low};
    for (std::size_t offset = 0; offset < (control ? 1u : priorities.size()); ++offset) {
        const auto cursor = (owner.priority_cursor + offset) % priorities.size();
        const auto priority = control ? TaskPriority::control : priorities[cursor];
        std::optional<std::size_t> result;
        for (std::size_t index = 0; index < owner.entries.size(); ++index) {
            const auto& entry = owner.entries[index];
            if (!entry.task || entry.priority != priority || (entry.prerequisite && entry.prerequisite->state() == OperationState::pending)) continue;
            if (!result || entry.order < owner.entries[*result].order) result = index;
        }
        if (result) return Choice{*result,control ? owner.priority_cursor : (cursor + 1) % priorities.size()};
    }
    return {};
}
// 保留执行及捕获资源析构期间的活动计费，不因槽已取出而提前归还预算。
// 步骤：转移任务并标记活动 → 隔离执行异常 → 销毁捕获 → 归还对应通路名额。
RunResult OwnerScheduler::execute(Owner& owner, std::size_t index) {
    auto entry = std::move(owner.entries[index]); owner.entries[index] = {};
    const bool control = entry.priority == TaskPriority::control;
    running_ = true; owner.running = true; auto result = RunResult::executed;
    try { entry.task(); } catch (...) { result = RunResult::failed; }
    entry = {}; --owner.used; if (control) --owner.control_used;
    owner.running = false; running_ = false; return result;
}
// 控制通路有独立预留和轮转，连续四步后给就绪普通任务一次机会。
// 步骤：拒绝重入 → 选择有预算的通路 → 按所有者份额筛选资格 → 更新优先级与控制份额 → 执行一步。
RunResult OwnerScheduler::run_one() {
    if (running_) throw std::logic_error("scheduler: recursive execution");
    for (std::size_t pass = 0; pass < 2; ++pass) {
        const bool control = (control_burst_ < 4) != (pass != 0);
        const std::size_t lane = control ? 1 : 0;
        for (std::size_t visited = 0; visited < slots_.size(); ++visited) {
            auto& cursor = cursor_[lane]; auto& remaining = remaining_[lane];
            auto* owner = slots_[cursor].empty() ? nullptr : &checked(slots_[cursor]);
            const auto choice = owner ? eligible(*owner,control) : std::nullopt;
            if (!choice) { remaining = 0; cursor = (cursor + 1) % slots_.size(); continue; }
            if (!remaining) remaining = owner->weight;
            if (!--remaining) cursor = (cursor + 1) % slots_.size();
            owner->priority_cursor = choice->next_priority;
            control_burst_ = control ? std::min<std::size_t>(4,control_burst_ + 1) : 0;
            return execute(*owner,choice->index);
        }
    }
    return RunResult::idle;
}
}
