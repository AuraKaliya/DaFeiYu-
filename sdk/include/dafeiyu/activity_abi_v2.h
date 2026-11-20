#pragma once
#include <dafeiyu/lifecycle_abi_v2.h>

#define DFY_V2_ACTIVITY_INTERFACE "dfy.activity"
#define DFY_V2_ACTIVITY_VERSION UINT32_C(1)
#define DFY_V2_ACTIVITY_MAX_WAIT_MS UINT32_C(30000)

#ifdef __cplusplus
extern "C" {
#endif
#pragma pack(push, 8)
typedef struct dfy_v2_activity_api {
    dfy_v2_api_header header;
    // 在原宿主线程执行一个有界步骤，可用已协商宿主服务；返回建议等候的毫秒数，零为下一轮调度。
    // 调用不得等待外部 I/O、模型或游戏，仍在途的工作由插件保存并在后续步骤继续推进。
    dfy_v2_status (DFY_V2_CALL *step)(dfy_v2_instance* instance, uint32_t* wait_ms, dfy_v2_error* error);
} dfy_v2_activity_api;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
