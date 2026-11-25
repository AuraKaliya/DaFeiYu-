#include "host_state.hpp"

namespace dfy::plugins::detail {
// 只有流已结束才归还宿主控制责任；读取结果及 C 客户端引用保持独立。
// 步骤：核对队列终态 → 找到资源记录 → 撤销通道登记并完成清理 → 清空宿主槽位。
void finish_reader(HostState& host, const std::shared_ptr<ReaderReference>& reader) {
    if (reader->queue_state() != events::QueueState::closed) return;
    for (auto& slot : host.readers) {
        if (slot.reader != reader) continue;
        host.resources.begin_cleanup(slot.resource);
        try {
            if (slot.queue) host.channel->remove(slot.queue);
            host.resources.finish_cleanup(slot.resource);
            slot = {};
        } catch (...) { host.resources.fail_cleanup(slot.resource); throw; }
        return;
    }
}
namespace {
// 在副作用前登记资源，任何失败都回收新队列及本次引用。
// 步骤：检查接纳与容量 → 准备资源 → 创建队列、控制器与引用 → 激活或回滚。
dfy_v2_status DFY_V2_CALL subscribe(dfy_v2_handle* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        if (!host.channel) return DFY_V2_UNSUPPORTED;
        if (host.resources.state() != runtime::OwnerState::open) return DFY_V2_BUSY;
        for (auto& slot : host.readers) {
            if (!slot.resource.empty()) continue;
            slot.resource = host.resources.prepare();
            runtime::Handle exposed{};
            try {
                slot.queue = host.channel->subscribe();
                slot.reader = std::make_shared<ReaderReference>(std::make_shared<events::SubscriptionReader>(slot.queue, host.request_capacity));
                exposed = host.references.insert(std::make_shared<HostReference>(HostReference{slot.reader}), false);
                host.resources.activate(slot.resource);
                *output = abi(exposed);
                return DFY_V2_OK;
            } catch (...) {
                try {
                    host.references.release(exposed);
                    if (slot.queue) { slot.queue->close(events::CloseMode::cancel); host.channel->remove(slot.queue); }
                    host.resources.begin_cleanup(slot.resource); host.resources.finish_cleanup(slot.resource); slot = {};
                } catch (...) { host.cleanup_failed = true; }
                throw;
            }
        }
        return DFY_V2_RESOURCE_EXHAUSTED;
    });
}
// 关闭一份交付并推进对应等待，不影响其他接收方或已交付数据。
dfy_v2_status DFY_V2_CALL close(dfy_v2_handle source, uint32_t mode, dfy_v2_error* error) {
    return boundary(error, [&](auto& host) {
        if (mode != DFY_V2_CLOSE_DRAIN && mode != DFY_V2_CLOSE_CANCEL) return DFY_V2_INVALID_ARGUMENT;
        const auto receiver = reader(host,source);
        receiver->subscription()->close(mode == DFY_V2_CLOSE_DRAIN ? events::CloseMode::drain : events::CloseMode::cancel);
        finish_reader(host, receiver);
        return DFY_V2_OK;
    });
}
// 返回队列进度副本，流关闭不冒充消费任务退出。
dfy_v2_status DFY_V2_CALL read(dfy_v2_handle source, dfy_v2_subscription_info* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error, [&](auto& host) {
        const auto reader = detail::reader(host,source)->subscription();
        dfy_v2_subscription_info info{};
        switch (reader->queue_state()) {
        case events::QueueState::open: info.state = DFY_V2_SUBSCRIPTION_OPEN; break;
        case events::QueueState::draining: info.state = DFY_V2_SUBSCRIPTION_DRAINING; break;
        case events::QueueState::closed: info.state = DFY_V2_SUBSCRIPTION_CLOSED; break;
        }
        if (const auto mode = reader->close_mode()) info.close_mode = *mode == events::CloseMode::drain ? DFY_V2_CLOSE_DRAIN : DFY_V2_CLOSE_CANCEL;
        info.queued = reader->queued();
        *output = info;
        return DFY_V2_OK;
    });
}
}
// 提供固定版本的独立订阅服务。
const dfy_v2_subscription_api& subscription_api() {
    static const dfy_v2_subscription_api api = {{sizeof(dfy_v2_subscription_api), 1}, subscribe, close, read};
    return api;
}
}
