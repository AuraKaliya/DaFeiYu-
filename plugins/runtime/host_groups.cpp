#include "host_state.hpp"

namespace dfy::plugins::detail {
namespace {
// 在宿主授权的组中建立成员接收方，失败回收本次登记而不关闭共享组。
// 步骤：检查授权与接纳状态 → 预登记资源 → 创建成员及引用 → 激活或逐项回滚。
dfy_v2_status DFY_V2_CALL join(dfy_v2_handle* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error,[&](auto& host) {
        if (!host.group) return DFY_V2_UNSUPPORTED;
        if (host.resources.state() != runtime::OwnerState::open || host.group->state() != events::QueueState::open) return DFY_V2_BUSY;
        for (auto& slot : host.readers) {
            if (!slot.resource.empty()) continue;
            slot.resource = host.resources.prepare(); runtime::Handle exposed{};
            try {
                slot.reader = std::make_shared<ReaderReference>(std::make_shared<events::GroupReader>(host.group,host.group_request_capacity));
                exposed = host.references.insert(std::make_shared<HostReference>(HostReference{slot.reader}),false);
                host.resources.activate(slot.resource); *output = abi(exposed);
                return DFY_V2_OK;
            } catch (...) {
                try {
                    host.references.release(exposed);
                    if (slot.reader) slot.reader->group_member()->close();
                    host.resources.begin_cleanup(slot.resource); host.resources.finish_cleanup(slot.resource); slot = {};
                } catch (...) { host.cleanup_failed = true; }
                throw;
            }
        }
        return DFY_V2_RESOURCE_EXHAUSTED;
    });
}
// 只结束当前接收方的成员责任，成功读取和普通数据引用保持原结果。
// 步骤：核对精确控制器种类 → 退出成员并推进等待 → 归还已结束的宿主资源。
dfy_v2_status DFY_V2_CALL leave(dfy_v2_handle source, dfy_v2_error* error) {
    return boundary(error,[&](auto& host) {
        const auto receiver = reader(host,source);
        receiver->group_member()->close();
        finish_reader(host,receiver);
        return DFY_V2_OK;
    });
}
// 返回本成员可观察的阶段和组待领数量，查询不驱动队列或取消。
// 步骤：清空输出 → 校验成员控制器 → 显式映射状态与模式 → 复制数量。
dfy_v2_status DFY_V2_CALL read(dfy_v2_handle source, dfy_v2_group_member_info* output, dfy_v2_error* error) {
    if (output) *output = {};
    if (!output) { if (error) error->length = 0; return DFY_V2_INVALID_ARGUMENT; }
    return boundary(error,[&](auto& host) {
        const auto member = reader(host,source)->group_member();
        dfy_v2_group_member_info info{};
        switch (member->queue_state()) {
        case events::QueueState::open: info.state = DFY_V2_GROUP_MEMBER_OPEN; break;
        case events::QueueState::draining: info.state = DFY_V2_GROUP_MEMBER_DRAINING; break;
        case events::QueueState::closed: info.state = DFY_V2_GROUP_MEMBER_CLOSED; break;
        }
        if (const auto mode = member->close_mode()) info.close_mode = *mode == events::CloseMode::drain ? DFY_V2_CLOSE_DRAIN : DFY_V2_CLOSE_CANCEL;
        info.queued = member->queued(); *output = info;
        return DFY_V2_OK;
    });
}
}
// 返回精确版本的成员服务，不暴露共享组的全局关闭或发布权。
const dfy_v2_group_api& group_api() {
    static const dfy_v2_group_api api = {{sizeof(dfy_v2_group_api),1},join,leave,read};
    return api;
}
}
