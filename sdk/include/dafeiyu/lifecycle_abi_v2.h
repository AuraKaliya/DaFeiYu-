#pragma once
#include <dafeiyu/plugin_abi_v2.h>
#define DFY_V2_LIFECYCLE_INTERFACE "dfy.lifecycle"
#define DFY_V2_LIFECYCLE_VERSION UINT32_C(1)
#define DFY_V2_STOP_PENDING UINT32_C(1)
#define DFY_V2_STOPPED UINT32_C(2)
#ifdef __cplusplus
extern "C" {
#endif
#pragma pack(push, 8)
typedef struct dfy_v2_instance dfy_v2_instance;
typedef struct dfy_v2_lifecycle_api {
    dfy_v2_api_header header;
    // 在宿主绑定中创建唯一实例；配置仅调用内借用，失败清空实例并回收插件私有部分分配。
    dfy_v2_status (DFY_V2_CALL *create)(dfy_v2_query_interface_fn host, dfy_v2_string_view config,
        dfy_v2_instance** instance, dfy_v2_error* error);
    // 启动一次；失败可留下需 request_stop 收尾的部分启动，不得由宿主直接销毁。
    dfy_v2_status (DFY_V2_CALL *start)(dfy_v2_instance* instance, dfy_v2_error* error);
    // 提交或重试停止，OK 只表示请求已受理；重复调用不得重放已成功副作用。
    dfy_v2_status (DFY_V2_CALL *request_stop)(dfy_v2_instance* instance, dfy_v2_error* error);
    // 非阻塞推进或查询停止，输出必需且失败清零；STOPPED 后保持终态且不再使用插件任务。
    dfy_v2_status (DFY_V2_CALL *poll_stop)(dfy_v2_instance* instance, uint32_t* state, dfy_v2_error* error);
    // 仅在 STOPPED 且宿主注册已静止后调用一次；分配方释放实例，不得抛出异常。
    void (DFY_V2_CALL *destroy)(dfy_v2_instance* instance);
} dfy_v2_lifecycle_api;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
