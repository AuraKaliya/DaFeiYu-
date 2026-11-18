#pragma once
#include <dafeiyu/lifecycle_abi_v2.h>
#define DFY_V2_COMPANION_INTERFACE "dfy.companion"
#define DFY_V2_COMPANION_VERSION UINT32_C(1)
#define DFY_V2_COMPANION_ROUTES_INTERFACE "dfy.companion.routes"
#define DFY_V2_COMPANION_ROUTES_VERSION UINT32_C(1)
#ifdef __cplusplus
extern "C" {
#endif
#pragma pack(push, 8)
// 宿主在调用期间复制本插件局部状态工具名及其可查询主题，二者均不授予执行权。
typedef struct dfy_v2_companion_description {
    uint32_t struct_size;
    uint32_t reserved;
    dfy_v2_string_view state_tool;
    dfy_v2_string_view state_topic;
} dfy_v2_companion_description;
typedef struct dfy_v2_companion_api {
    dfy_v2_api_header header;
    // 限原活动实例及宿主线程；检查输出布局，不启动会话，字符串仅借用至下次插件调用。
    dfy_v2_status (DFY_V2_CALL *describe)(dfy_v2_instance* instance, dfy_v2_companion_description* out, dfy_v2_error* error);
} dfy_v2_companion_api;
typedef struct dfy_v2_companion_routes_api {
    dfy_v2_api_header header;
    // 在原活动实例内查询指定业务用途的局部工具名，未注册返回 NOT_FOUND，借用仅限本次调用。
    dfy_v2_status (DFY_V2_CALL *resolve)(dfy_v2_instance* instance, dfy_v2_string_view purpose, dfy_v2_string_view* local_tool,
        dfy_v2_error* error);
} dfy_v2_companion_routes_api;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
