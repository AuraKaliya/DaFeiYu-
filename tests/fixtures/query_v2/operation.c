#include <stddef.h>
#pragma pack(push, 1)
#include "api.h"
struct operation_packing_probe { char tag; uint64_t value; };
#pragma pack(pop)
_Static_assert(sizeof(struct operation_packing_probe) == 9, "packing restored");
_Static_assert(sizeof(dfy_v2_operation_snapshot) == 4 && _Alignof(dfy_v2_operation_snapshot) == 4 &&
    offsetof(dfy_v2_operation_snapshot, state) == 0, "snapshot layout");
_Static_assert(sizeof(dfy_v2_operation_state_api) == 16 && _Alignof(dfy_v2_operation_state_api) == 8 &&
    offsetof(dfy_v2_operation_state_api, read) == 8, "state service layout");
_Static_assert(sizeof(dfy_test_operation_api) == 16 && offsetof(dfy_test_operation_api, read) == 8, "fixture layout");

// 在纯 C DLL 内读取宿主状态，保留查询失败与操作终态的区别。
// 步骤：清空输出 → 校验宿主服务布局及入口 → 借用操作引用读取快照。
static dfy_v2_status DFY_V2_CALL read_state(const dfy_v2_operation_state_api* states,
    dfy_v2_handle operation, dfy_v2_operation_snapshot* snapshot, dfy_v2_error* error) {
    if (snapshot) snapshot->state = 0;
    if (error) error->length = 0;
    if (!states || !snapshot) return DFY_V2_INVALID_ARGUMENT;
    if (states->header.struct_size != sizeof(*states) ||
        states->header.interface_version != DFY_V2_OPERATION_STATE_VERSION) return DFY_V2_UNSUPPORTED;
    if (!states->read) return DFY_V2_INVALID_ARGUMENT;

    return states->read(operation, snapshot, error);
}
const dfy_test_operation_api dfy_test_operation = {{sizeof(dfy_test_operation_api), 1}, read_state};

_Static_assert(sizeof(dfy_v2_cancel_snapshot) == 4 && _Alignof(dfy_v2_cancel_snapshot) == 4 &&
    offsetof(dfy_v2_cancel_snapshot, reason) == 0, "cancel snapshot layout");
_Static_assert(sizeof(dfy_v2_operation_cancel_api) == 24 && _Alignof(dfy_v2_operation_cancel_api) == 8 &&
    offsetof(dfy_v2_operation_cancel_api, request) == 8 && offsetof(dfy_v2_operation_cancel_api, read) == 16,
    "cancel service layout");
_Static_assert(sizeof(dfy_test_cancel_api) == 24 && offsetof(dfy_test_cancel_api, read) == 16, "cancel fixture layout");

// 核对取消服务的精确布局和必需入口，不调用宿主业务。
static dfy_v2_status validate_cancel(const dfy_v2_operation_cancel_api* cancels) {
    if (!cancels) return DFY_V2_INVALID_ARGUMENT;
    if (cancels->header.struct_size != sizeof(*cancels) ||
        cancels->header.interface_version != DFY_V2_OPERATION_CANCEL_VERSION) return DFY_V2_UNSUPPORTED;
    return cancels->request && cancels->read ? DFY_V2_OK : DFY_V2_INVALID_ARGUMENT;
}
// 校验服务并提出协作取消，回执不表示操作已经退出。
// 步骤：清空输出 → 校验参数及宿主服务 → 转交取消请求。
static dfy_v2_status DFY_V2_CALL request_cancel(const dfy_v2_operation_cancel_api* cancels,
    dfy_v2_handle operation, uint32_t reason, uint32_t* disposition, dfy_v2_error* error) {
    dfy_v2_status status = validate_cancel(cancels);
    if (disposition) *disposition = 0;
    if (error) error->length = 0;
    if (!disposition) return DFY_V2_INVALID_ARGUMENT;
    if (status != DFY_V2_OK) return status;
    return cancels->request(operation, reason, disposition, error);
}
// 校验服务后读取取消原因副本，错误不返回旧输出。
// 步骤：清空输出 → 校验参数及宿主服务 → 读取取消快照。
static dfy_v2_status DFY_V2_CALL read_cancel(const dfy_v2_operation_cancel_api* cancels,
    dfy_v2_handle operation, dfy_v2_cancel_snapshot* snapshot, dfy_v2_error* error) {
    dfy_v2_status status = validate_cancel(cancels);
    if (snapshot) snapshot->reason = 0;
    if (error) error->length = 0;
    if (!snapshot) return DFY_V2_INVALID_ARGUMENT;
    if (status != DFY_V2_OK) return status;
    return cancels->read(operation, snapshot, error);
}
const dfy_test_cancel_api dfy_test_cancel = {{sizeof(dfy_test_cancel_api), 1}, request_cancel, read_cancel};
