#pragma once
#include <dafeiyu/handle_registry.hpp>
#include <dafeiyu/fifo_executor.hpp>
#include <dafeiyu/operation.hpp>
#include <optional>

namespace dfy::runtime {
// 单宿主线程的有界加权轮转，等待只占记录，不消耗执行份额；不抢占原生调用。
class OwnerScheduler {
  public:
    // 固定所有者名额，所有者权重为 1～64，任务容量在登记时明确指定。
    explicit OwnerScheduler(std::size_t owners);
    // 禁止复制调度责任和所有者身份。
    OwnerScheduler(const OwnerScheduler&) = delete;
    // 禁止赋值或隐式移动正在使用的调度器。
    OwnerScheduler& operator=(const OwnerScheduler&) = delete;
    // 登记拥有独立正任务容量的所有者，失败不发布半个所有者。
    Handle add_owner(std::size_t weight, std::size_t capacity);
    // 停止该所有者的新任务接纳，保留排队及运行责任供继续推进。
    void stop(Handle owner);
    // 只移除已经停止且无排队或运行任务的所有者，旧身份随即失效。
    void remove(Handle owner);
    // 接纳一次执行步骤，前置操作未终结时不具备执行资格；提交不调用任务。
    void post(Handle owner, FifoExecutor::Task task, std::shared_ptr<const Operation> prerequisite = {});
    // 先检查资格再按所有者权重取一项；同所有者按接纳顺序，递归推进拒绝，异常隔离。
    RunResult run_one();
    // 返回该所有者排队加运行的占用数，异步等待和活动步骤均计入容量。
    std::size_t size(Handle owner) const { return checked(owner).used; }
  private:
    struct Entry { FifoExecutor::Task task; std::shared_ptr<const Operation> prerequisite; std::uint64_t order = 0; };
    struct Owner {
        // 固定权重和完整任务槽，尚未产生任何可执行工作。
        Owner(std::size_t value, std::size_t capacity) : entries(capacity), weight(value) {}
        std::vector<Entry> entries;
        std::size_t weight, used = 0;
        bool accepting = true, running = false;
    };
    // 查找本所有者最早且前置已终结的任务，不改变执行状态。
    std::optional<std::size_t> eligible(const Owner& owner) const noexcept;
    struct OwnerReference { std::shared_ptr<Owner> owner; };
    // 通过只读引用表取得由本调度器独占修改的所有者记录。
    Owner& checked(Handle owner) const { return *owners_.get(owner).owner; }
    HandleRegistry<OwnerReference> owners_;
    std::vector<Handle> slots_;
    std::size_t cursor_ = 0, remaining_ = 0;
    std::uint64_t next_order_ = 1;
    bool running_ = false;
};
}
