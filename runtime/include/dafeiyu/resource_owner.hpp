#pragma once
#include <dafeiyu/handle_registry.hpp>

namespace dfy::runtime {
enum class OwnerState { open, closing, closed };
enum class ResourceState { preparing, active, cleaning, cleanup_failed };
class ResourceUse;

// 一个宿主所有者实例的有界资源登记；每实例独立签发域，所有操作限同一宿主线程。
// 登记句柄仅供宿主推进清理，不作为插件可直接释放的普通数据引用发布。
class ResourceOwner {
  public:
    // 建立资源容量及各自的入向／出向使用上限；缺省零不允许建立使用关系。
    explicit ResourceOwner(std::size_t capacity, std::size_t use_capacity = 0);
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
    // 为指定消费者取得一份移动式使用凭证，绑定本实例资源及双方具体签发域。
    ResourceUse acquire(Handle resource, ResourceOwner& consumer);
    // 关闭资源及使用接纳，重复调用保持 Closing 或 Closed，不自动开始清理。
    void stop_accepting() noexcept { if (control_->state == OwnerState::open) control_->state = OwnerState::closing; }
    // 只有已停止接纳且全部登记与使用已结束才能确认 Closed；重复关闭无副作用。
    void finish_close();
    // 返回所有者状态副本，不代表底层 DLL 已卸载。
    OwnerState state() const noexcept { return control_->state; }
    // 返回指定资源的阶段副本，跨实例和过期句柄由引用表拒绝。
    ResourceState resource_state(Handle resource) const { return checked(resource).state; }
    // 返回当前仍需跟踪的资源数，清理失败项也计入。
    std::size_t size() const noexcept { return resources_.size(); }
    // 返回本实例的资源签发域，不作为插件包 ID 或可持久化标识。
    DomainId domain() const noexcept { return resources_.domain(); }
    // 返回其他工作当前持有的资源使用数，同所有者的自用也计入。
    std::size_t incoming_uses() const noexcept { return control_->incoming; }
    // 返回本实例持有的提供方使用关系数，结束前不能确认所有者关闭。
    std::size_t outgoing_uses() const noexcept { return control_->outgoing; }
    // 返回指定资源的当前使用数，不代表正在执行的原生调用数。
    std::size_t resource_users(Handle resource) const { return checked(resource).users; }

  private:
    friend class ResourceUse;
    struct Record { ResourceState state = ResourceState::preparing; std::size_t users = 0; };
    struct Reference { std::shared_ptr<Record> record; };
    struct Control {
        DomainId domain;
        std::size_t capacity;
        OwnerState state = OwnerState::open;
        std::size_t incoming = 0, outgoing = 0;
    };
    // 校验实例与代次后取得宿主内部记录，不向消费者暴露可修改状态。
    Record& checked(Handle resource) const { return *resources_.get(resource).record; }
    HandleRegistry<Reference> resources_;
    std::shared_ptr<Control> control_;
};

// 一份具体资源使用关系；不可复制，须在同宿主线程最后一次实际使用结束后释放。
class ResourceUse {
  public:
    // 创建不持有任何使用责任的空凭证。
    ResourceUse() = default;
    // 归还本次使用计量，不释放提供方实际资源或表示业务成功。
    ~ResourceUse() { reset(); }
    // 禁止隐式复制使用责任，另一份使用必须重新申请并计费。
    ResourceUse(const ResourceUse&) = delete;
    // 禁止复制赋值覆盖使用责任。
    ResourceUse& operator=(const ResourceUse&) = delete;
    // 移动转交一次使用，来源随即变为空凭证。
    ResourceUse(ResourceUse&& other) noexcept;
    // 归还旧使用并移动接收新绑定，自移动无操作。
    ResourceUse& operator=(ResourceUse&& other) noexcept;
    // 结束一次使用，重复释放空凭证无操作。
    void reset() noexcept;
    // 判断是否仍保持实际使用关系。
    explicit operator bool() const noexcept { return static_cast<bool>(record_); }
    // 返回绑定的资源控制标识，空凭证返回全零；不赋予资源清理权限。
    Handle resource() const noexcept { return resource_; }
    // 返回绑定的具体提供方域，空凭证为零。
    DomainId provider_domain() const noexcept { return provider_ ? provider_->domain : 0; }
    // 返回绑定的具体消费者域，新实例不会自动替换旧绑定。
    DomainId consumer_domain() const noexcept { return consumer_ ? consumer_->domain : 0; }
  private:
    friend class ResourceOwner;
    // 保存已经计费的具体绑定，不执行分配或外部回调。
    ResourceUse(std::shared_ptr<ResourceOwner::Control> provider, std::shared_ptr<ResourceOwner::Control> consumer,
        std::shared_ptr<ResourceOwner::Record> record, Handle resource) noexcept
        : provider_(std::move(provider)), consumer_(std::move(consumer)), record_(std::move(record)), resource_(resource) {}
    std::shared_ptr<ResourceOwner::Control> provider_, consumer_;
    std::shared_ptr<ResourceOwner::Record> record_;
    Handle resource_;
};
}
