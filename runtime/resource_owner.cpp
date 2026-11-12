#include <dafeiyu/resource_owner.hpp>

namespace dfy::runtime {
// 在副作用之前登记清理责任，接纳关闭或容量不足时保持已有记录。
// 步骤：检查接纳状态 → 创建 Preparing 记录 → 签发不可共享控制引用。
Handle ResourceOwner::prepare() {
    if (state_ != OwnerState::open) throw std::logic_error("owner: resource admission closed");
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
    checked(resource).state = ResourceState::cleaning;
}

// 保留失败清理的责任，不能据此归还资源槽位或完成所有者关闭。
void ResourceOwner::fail_cleanup(Handle resource) {
    auto& record = checked(resource);
    if (record.state != ResourceState::cleaning) throw std::logic_error("resource: cleanup not started");
    record.state = ResourceState::cleanup_failed;
}

// 确认清理已完成后撤销登记，旧标识不能再用于重试或新资源。
void ResourceOwner::finish_cleanup(Handle resource) {
    if (checked(resource).state != ResourceState::cleaning) throw std::logic_error("resource: cleanup not active");
    resources_.release(resource);
}

// 在所有登记均已收尾后确认关闭，失败保持当前关闭进度。
// 步骤：复用 Closed 结果 → 核对停止接纳及未完资源 → 固定 Closed 状态。
void ResourceOwner::finish_close() {
    if (state_ == OwnerState::closed) return;
    if (state_ != OwnerState::closing || resources_.size()) throw std::logic_error("owner: resources not closed");
    state_ = OwnerState::closed;
}
}
