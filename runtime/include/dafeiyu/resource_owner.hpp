#pragma once
#include <dafeiyu/handle_registry.hpp>

namespace dfy::runtime {
enum class OwnerState { open, closing, closed };
enum class ResourceState { preparing, active, cleaning, cleanup_failed };

// 一个宿主所有者实例的有界资源登记；每实例独立签发域，所有操作限同一宿主线程。
// 登记句柄仅供宿主推进清理，不作为插件可直接释放的普通数据引用发布。
class ResourceOwner {
  public:
    // 建立独立域和资源容量，副作用执行前须先取得登记。
    explicit ResourceOwner(std::size_t capacity) : resources_(capacity) {}
    // 仅释放登记存储；宿主必须先完成实际资源清理并关闭所有者。
    ~ResourceOwner() = default;
    // 禁止复制具体实例的资源归属及签发域。
    ResourceOwner(const ResourceOwner&) = delete;
    // 禁止赋值或移动所有者实例。
    ResourceOwner& operator=(const ResourceOwner&) = delete;
    // 在仍接纳资源时创建 Preparing 登记，失败不得开始对应副作用。
    Handle prepare();
    // 记录已准备资源实际建立成功；不重新开放已经关闭的接纳入口。
    void activate(Handle resource);
    // 进入或重试清理，重复进入 Cleaning 无副作用；不执行实际释放。
    void begin_cleanup(Handle resource);
    // 记录清理失败并保留登记，须显式重试后才能确认完成。
    void fail_cleanup(Handle resource);
    // 在实际清理完成后结束 Cleaning 登记，释放后所有旧别名立即失效。
    void finish_cleanup(Handle resource);
    // 关闭资源接纳，重复调用保持 Closing 或 Closed，不自动开始清理。
    void stop_accepting() noexcept { if (state_ == OwnerState::open) state_ = OwnerState::closing; }
    // 只有已停止接纳且全部登记已结束才能确认 Closed；重复关闭无副作用。
    void finish_close();
    // 返回所有者状态副本，不代表底层 DLL 已卸载。
    OwnerState state() const noexcept { return state_; }
    // 返回指定资源的阶段副本，跨实例和过期句柄由引用表拒绝。
    ResourceState resource_state(Handle resource) const { return checked(resource).state; }
    // 返回当前仍需跟踪的资源数，清理失败项也计入。
    std::size_t size() const noexcept { return resources_.size(); }
    // 返回本实例的资源签发域，不作为插件包 ID 或可持久化标识。
    DomainId domain() const noexcept { return resources_.domain(); }

  private:
    struct Record { ResourceState state = ResourceState::preparing; };
    struct Reference { std::shared_ptr<Record> record; };
    // 校验实例与代次后取得宿主内部记录，不向消费者暴露可修改状态。
    Record& checked(Handle resource) const { return *resources_.get(resource).record; }
    HandleRegistry<Reference> resources_;
    OwnerState state_ = OwnerState::open;
};
}
