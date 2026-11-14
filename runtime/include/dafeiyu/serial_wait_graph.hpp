#pragma once
#include <dafeiyu/serial_domain.hpp>
#include <dafeiyu/handle_registry.hpp>

namespace dfy::runtime {
// 同宿主线程的显式等待依赖图，只检查已声明的顺序域等待，不推断外部锁或 I/O 依赖。
class SerialWaitGraph {
  public:
    // 建立正登记容量，未归还的完成依赖也占名额，防止元数据无界增长。
    explicit SerialWaitGraph(std::size_t capacity) : references_(capacity), slots_(capacity) {}
    // 释放依赖元数据，不结束实际处理，也不释放顺序域占用。
    ~SerialWaitGraph() = default;
    // 禁止复制图身份和依赖登记责任。
    SerialWaitGraph(const SerialWaitGraph&) = delete;
    // 禁止赋值或隐式移动仍被使用的图。
    SerialWaitGraph& operator=(const SerialWaitGraph&) = delete;
    // 为已授予处理登记等待；目标已释放或取消时返回空值，自等待或成环时原子拒绝。
    Handle add(std::shared_ptr<const SerialRequest> owner, std::shared_ptr<const SerialRequest> target);
    // 归还一份依赖登记，空值无操作，错误或已经归还的身份拒绝。
    void release(Handle dependency);
    // 判断登记是否仍约束当前等待，已完成目标或不再持有域的来源不参与后续环检查。
    bool active(Handle dependency) const;
    // 返回仍未归还的登记数，不等于当前有效边数。
    std::size_t size() const noexcept { return references_.size(); }
  private:
    struct Dependency {
        std::weak_ptr<const SerialRequest> owner, target;
        DomainId from, to;
        // 只在来源仍占用域且目标处理未结束时保留等待约束，不延长请求记录寿命。
        bool active() const noexcept {
            const auto source = owner.lock(), destination = target.lock();
            return source && destination && (source->phase() == SerialPhase::granted || source->phase() == SerialPhase::running) &&
                (destination->phase() == SerialPhase::waiting || destination->phase() == SerialPhase::granted || destination->phase() == SerialPhase::running);
        }
    };
    // 检查加入 from→to 后是否形成环，分配失败不改变已有登记。
    bool creates_cycle(DomainId from, DomainId to) const;
    HandleRegistry<const Dependency> references_;
    std::vector<Handle> slots_;
};
}
