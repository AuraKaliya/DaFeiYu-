#include <dafeiyu/resource_owner.hpp>

namespace dfy::runtime {
// 为资源签发域创建独立的使用控制记录，失败不返回半构造所有者。
// 步骤：建立资源表 → 绑定域及入出向容量 → 保持空的使用计量。
ResourceOwner::ResourceOwner(std::size_t capacity, std::size_t use_capacity)
    : resources_(capacity), control_(std::make_shared<Control>(Control{resources_.domain(), use_capacity})) {}

// 在副作用之前登记清理责任，接纳关闭或容量不足时保持已有记录。
// 步骤：检查接纳状态 → 创建 Preparing 记录 → 签发不可共享控制引用。
Handle ResourceOwner::prepare() {
    if (control_->state != OwnerState::open) throw std::logic_error("owner: resource admission closed");
    return resources_.insert(std::make_shared<Reference>(Reference{std::make_shared<Record>()}), false);
}

// 将准备中的登记记为已建立，不改变所有者关闭进度。
void ResourceOwner::activate(Handle resource) {
    auto& record = checked(resource);
    if (record.state != ResourceState::preparing) throw std::logic_error("resource: not preparing");
    record.state = ResourceState::active;
}

// 开始或重试实际清理的跟踪，允许处理部分初始化失败的资源。
void ResourceOwner::begin_cleanup(Handle resource) {
    auto& record = checked(resource);
    if (record.users) throw std::logic_error("resource: still in use");
    record.state = ResourceState::cleaning;
}

// 保留失败清理的责任，不能据此归还资源槽位或完成所有者关闭。
void ResourceOwner::fail_cleanup(Handle resource) {
    auto& record = checked(resource);
    if (record.state != ResourceState::cleaning) throw std::logic_error("resource: cleanup not started");
    record.state = ResourceState::cleanup_failed;
}

// 确认清理已完成后撤销登记，旧标识不能再用于重试或新资源。
void ResourceOwner::finish_cleanup(Handle resource) {
    const auto& record = checked(resource);
    if (record.state != ResourceState::cleaning || record.users) throw std::logic_error("resource: cleanup not complete");
    resources_.release(resource);
}

// 在所有登记均已收尾后确认关闭，失败保持当前关闭进度。
// 步骤：复用 Closed 结果 → 核对停止接纳及未完资源 → 固定 Closed 状态。
void ResourceOwner::finish_close() {
    if (control_->state == OwnerState::closed) return;
    if (control_->state != OwnerState::closing || resources_.size() || control_->incoming || control_->outgoing) {
        throw std::logic_error("owner: resources or uses not closed");
    }
    control_->state = OwnerState::closed;
}

// 建立绑定双方实例的使用关系，任一拒绝均不改变计量。
// 步骤：校验资源与双方接纳状态 → 核对入出向容量 → 计费并转交移动式凭证。
ResourceUse ResourceOwner::acquire(Handle resource, ResourceOwner& consumer) {
    const auto& record = resources_.get(resource).record;
    if (control_->state != OwnerState::open || consumer.control_->state != OwnerState::open || record->state != ResourceState::active) {
        throw std::logic_error("resource: use admission closed");
    }
    if (control_->incoming == control_->capacity || consumer.control_->outgoing == consumer.control_->capacity) {
        throw std::length_error("resource: use capacity exhausted");
    }
    ++control_->incoming;
    ++consumer.control_->outgoing;
    ++record->users;
    return ResourceUse(control_, consumer.control_, record, resource);
}

// 移动现有使用关系而不重新计费，来源标识清空。
ResourceUse::ResourceUse(ResourceUse&& other) noexcept
    : provider_(std::move(other.provider_)), consumer_(std::move(other.consumer_)), record_(std::move(other.record_)),
      resource_(std::exchange(other.resource_, {})) {}

// 转交使用责任，释放被覆盖的旧关系且不重复增加新关系计量。
// 步骤：拒绝自移动的重复处理 → 归还旧关系 → 转移持有并清空来源标识。
ResourceUse& ResourceUse::operator=(ResourceUse&& other) noexcept {
    if (this == &other) return *this;
    reset();
    provider_ = std::move(other.provider_);
    consumer_ = std::move(other.consumer_);
    record_ = std::move(other.record_);
    resource_ = std::exchange(other.resource_, {});
    return *this;
}

// 归还双方和资源的使用计量，不推进清理或修改业务状态。
// 步骤：忽略空凭证 → 扣减三处计量 → 释放持有并清空标识。
void ResourceUse::reset() noexcept {
    if (!record_) return;
    --record_->users;
    --provider_->incoming;
    --consumer_->outgoing;
    record_.reset();
    provider_.reset();
    consumer_.reset();
    resource_ = {};
}
}
