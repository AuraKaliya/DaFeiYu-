#include "host_state.hpp"

namespace dfy::plugins::detail {
// 宿主捕获的回调支撑，独立保持代码、操作借用与先登记的清理责任。
struct CallbackSupport {
    // 先取得代码凭证再登记资源，任一步失败都不留下外部回调。
    // 步骤：保持宿主弱关联 → 取得代码持有 → 准备资源登记。
    CallbackSupport(const std::shared_ptr<HostState>& state, dfy_v2_completion_fn function, void* context)
        : owner(state), code(state->code.retain()), resource(state->resources.prepare()), callback(function), user(context) { ++state->callback_pins; }
    // 结束借用和资源登记；契约失败单独记录，不从析构传播异常。
    // 步骤：取得存活宿主 → 释放操作借用 → 完成资源清理或记录失败。
    ~CallbackSupport() {
        if (auto state = owner.lock()) {
            --state->callback_pins;
            try {
                state->references.release(borrowed);
                state->resources.begin_cleanup(resource);
                state->resources.finish_cleanup(resource);
            } catch (...) { state->cleanup_failed = true; }
        }
    }
    // 绑定实际宿主和活动代码后调用 C 消费者，保护持续到原生入口返回。
    // 步骤：保持宿主 → 绑定上下文 → 建立活动保护 → 借用完整操作调用消费者。
    void deliver() {
        const auto state = owner.lock();
        if (!state) return;
        auto binding = HostServices::Binding(state);
        auto active = code.enter();
        callback(user, abi(borrowed));
    }
    std::weak_ptr<HostState> owner;
    platform::CodeLease code;
    runtime::Handle resource, borrowed{};
    dfy_v2_completion_fn callback;
    void* user;
};
namespace {
// 接受一次性 C 通知，并在全部准备成功前不暴露登记引用。
// 步骤：验证参数与能力 → 先登记资源并保留操作 → 预留客户端引用 → 登记原生通知或完整回滚。
dfy_v2_status DFY_V2_CALL subscribe(dfy_v2_handle operation, dfy_v2_completion_fn callback,
    void* user, dfy_v2_handle* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (error) error->length = 0;
    if (!output || !callback) return DFY_V2_INVALID_ARGUMENT;
    return boundary(error, [&](auto& host) {
        if (!host.notifications) return DFY_V2_UNSUPPORTED;
        if (host.resources.state() != runtime::OwnerState::open) return DFY_V2_BUSY;
        const auto source = request(host, operation);
        auto support = std::make_shared<CallbackSupport>(host.shared_from_this(), callback, user);
        support->borrowed = host.references.retain(native(operation));
        auto registration = std::make_shared<NotificationReference>();
        const auto exposed = host.references.insert(std::make_shared<HostReference>(HostReference{registration}), false);
        try {
            registration->registration = host.notifications->subscribe(source->operation(),
                [support](const auto&) { support->deliver(); });
            host.resources.activate(support->resource);
        } catch (...) {
            if (!registration->registration.empty()) {
                host.notifications->revoke(registration->registration);
                host.notifications->release(registration->registration);
            }
            host.references.release(exposed);
            throw;
        }
        *output = abi(exposed);
        return DFY_V2_OK;
    });
}
// 读取登记状态，Delivered 仅表示回调及其捕获已经收尾。
dfy_v2_status DFY_V2_CALL read(dfy_v2_handle source, dfy_v2_notification_snapshot* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        const auto registration = std::get<std::shared_ptr<NotificationReference>>(host.references.get(native(source)).object);
        switch (host.notifications->read(registration->registration)) {
        case runtime::NotificationState::pending: output->state = DFY_V2_NOTIFICATION_PENDING; break;
        case runtime::NotificationState::running: output->state = DFY_V2_NOTIFICATION_RUNNING; break;
        case runtime::NotificationState::delivered: output->state = DFY_V2_NOTIFICATION_DELIVERED; break;
        case runtime::NotificationState::revoked: output->state = DFY_V2_NOTIFICATION_REVOKED; break;
        }
        return DFY_V2_OK;
    });
}
// 撤销单个等待登记，不取消读取或影响其他等待者。
dfy_v2_status DFY_V2_CALL revoke(dfy_v2_handle source, dfy_v2_error* error) {
    return boundary(error, [&](auto& host) {
        const auto registration = std::get<std::shared_ptr<NotificationReference>>(host.references.get(native(source)).object);
        return host.notifications->revoke(registration->registration) == runtime::RevokeResult::busy ? DFY_V2_BUSY : DFY_V2_OK;
    });
}
}
// 提供已实现且受代码与资源约束的通知表。
const dfy_v2_operation_notify_api& notification_api() {
    static const dfy_v2_operation_notify_api api = {{sizeof(dfy_v2_operation_notify_api), 1}, subscribe, read, revoke};
    return api;
}
}
