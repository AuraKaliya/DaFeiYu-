#pragma once
#include <dafeiyu/event_abi_v2.h>
#define DFY_V2_GROUP_INTERFACE "dfy.groups"
#define DFY_V2_GROUP_VERSION UINT32_C(1)
#define DFY_V2_GROUP_MEMBER_OPEN UINT32_C(1)
#define DFY_V2_GROUP_MEMBER_DRAINING UINT32_C(2)
#define DFY_V2_GROUP_MEMBER_CLOSED UINT32_C(3)
#ifdef __cplusplus
extern "C" {
#endif
#pragma pack(push, 8)
// 本成员接收状态副本，退出后 queued 为零；Closed 不表示已交付数据或 DLL 已释放。
typedef struct dfy_v2_group_member_info { uint32_t state; uint32_t close_mode; uint64_t queued; } dfy_v2_group_member_info;
// 释放接收方引用不退出成员；leave 或宿主停止才结束登记，已交付数据按原持有独立释放。
typedef struct dfy_v2_group_api {
    dfy_v2_api_header header;
    // 在同宿主线程绑定内加入宿主授予的组，返回不可共享接收方；输出必需，失败清零且无半成员。
    dfy_v2_status (DFY_V2_CALL *join)(dfy_v2_handle* reader, dfy_v2_error* error);
    // 只退出当前上下文的成员，不关闭共享组、不重写完成读取；重复退出成功，error 可空。
    dfy_v2_status (DFY_V2_CALL *leave)(dfy_v2_handle reader, dfy_v2_error* error);
    // 只读本成员接收状态，不推进读取；输出必需且失败全零，已交付数据仍按原引用释放。
    dfy_v2_status (DFY_V2_CALL *read)(dfy_v2_handle reader, dfy_v2_group_member_info* output, dfy_v2_error* error);
} dfy_v2_group_api;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
