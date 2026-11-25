#pragma once
#include <dafeiyu/handle_registry.hpp>
#include <dafeiyu/fifo_executor.hpp>
#include <dafeiyu/operation.hpp>
#include <optional>
#include <array>

namespace dfy::runtime {
enum class TaskPriority { high, normal, low, control };
// 单宿主线程的有界加权轮转，等待只占记录，不消耗执行份额；不抢占原生调用。
class OwnerScheduler {
  public:
    // 固定所有者名额，所有者权重为 1～64，任务容量在登记时明确指定。
    explicit OwnerScheduler(std::size_t owners);
    // 禁止复制调度责任和所有者身份。
    OwnerScheduler(const OwnerScheduler&) = delete;
    // 禁止赋值或隐式移动正在使用的调度器。
    OwnerScheduler& operator=(const OwnerScheduler&) = delete;
    // 登记普通任务容量及独立控制预留；预留可为零，普通工作不能使用预留槽。
    Handle add_owner(std::size_t weight, std::size_t capacity, std::size_t control_reserve = 0);
    // 停止该所有者的新任务接纳，保留排队及运行责任供继续推进。
    void stop(Handle owner);
    // 只移除已经停止且无排队或运行任务的所有者，旧身份随即失效。
    void remove(Handle owner);
    // 接纳一次执行步骤，前置操作未终结时不具备执行资格；提交不调用任务。
    void post(Handle owner, FifoExecutor::Task task, std::shared_ptr<const Operation> prerequisite = {}, TaskPriority priority = TaskPriority::normal);
    // 先筛选资格；普通优先级按 4∶2∶1 份额，控制连续最多四步后给普通工作一次机会。
    RunResult run_one();
    // 返回指定通路可接纳的名额，包含运行占用；停止接纳后为零。
    std::size_t available(Handle owner, TaskPriority priority = TaskPriority::normal) const;
    // 返回该所有者排队加运行的占用数，异步等待和活动步骤均计入容量。
    std::size_t size(Handle owner) const { return checked(owner).used; }
  private:
    struct Entry { FifoExecutor::Task task; std::shared_ptr<const Operation> prerequisite; std::uint64_t order = 0; TaskPriority priority = TaskPriority::normal; };
    struct Owner {
        // 固定权重和完整任务槽，尚未产生任何可执行工作。
        Owner(std::size_t value, std::size_t capacity, std::size_t reserve)
            : entries(capacity + reserve), weight(value), ordinary_capacity(capacity), control_capacity(reserve) {}
        std::vector<Entry> entries;
        std::size_t weight, used = 0;
        std::size_t ordinary_capacity, control_capacity, control_used = 0, priority_cursor = 0;
        bool accepting = true, running = false;
    };
    struct Choice { std::size_t index, next_priority; };
    // 筛选有资格的控制或普通任务，普通优先级有固定份额，同优先级按接纳顺序。
    std::optional<Choice> eligible(const Owner& owner, bool control) const noexcept;
    // 执行单个步骤并归还对应通路的活动占用，异常不影响其他任务。
    RunResult execute(Owner& owner, std::size_t index);
    struct OwnerReference { std::shared_ptr<Owner> owner; };
    // 通过只读引用表取得由本调度器独占修改的所有者记录。
    Owner& checked(Handle owner) const { return *owners_.get(owner).owner; }
    HandleRegistry<OwnerReference> owners_;
    std::vector<Handle> slots_;
    std::array<std::size_t,2> cursor_{}, remaining_{};
    std::size_t control_burst_ = 0;
    std::uint64_t next_order_ = 1;
    bool running_ = false;
};
}
