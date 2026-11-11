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
