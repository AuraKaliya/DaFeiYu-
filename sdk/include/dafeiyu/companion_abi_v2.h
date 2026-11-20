#pragma once
#include <dafeiyu/lifecycle_abi_v2.h>
#define DFY_V2_COMPANION_INTERFACE "dfy.companion"
#define DFY_V2_COMPANION_VERSION UINT32_C(1)
#define DFY_V2_COMPANION_ROUTES_INTERFACE "dfy.companion.routes"
#define DFY_V2_COMPANION_ROUTES_VERSION UINT32_C(1)
#define DFY_V2_COMPANION_SKILLS_INTERFACE "dfy.companion.skills"
#define DFY_V2_COMPANION_SKILLS_VERSION UINT32_C(1)
#define DFY_V2_COMPANION_PARTNER_INTERFACE "dfy.companion.partner"
#define DFY_V2_COMPANION_PARTNER_VERSION UINT32_C(1)
#define DFY_V2_PARTNER_CLOSED UINT32_C(1)
#define DFY_V2_PARTNER_LISTENING UINT32_C(2)
#define DFY_V2_PARTNER_STOPPING UINT32_C(3)
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
typedef struct dfy_v2_companion_skills_api {
    dfy_v2_api_header header;
    // 限原实例宿主线程；从零枚举接收宿主上下文信封的模型技能，末尾返回 NOT_FOUND，名称在调用内复制。
    dfy_v2_status (DFY_V2_CALL *at)(dfy_v2_instance* instance, uint32_t index, dfy_v2_string_view* local_tool, dfy_v2_error* error);
} dfy_v2_companion_skills_api;
typedef struct dfy_v2_partner_status {
    uint32_t phase, ready;
    uint64_t session, cursor, departed, settled;
    dfy_v2_string_view world, player;
} dfy_v2_partner_status;
typedef struct dfy_v2_companion_partner_api {
    dfy_v2_api_header header;
    // 显式开启插件拥有的伙伴循环，不等待游戏或模型首次响应。
    dfy_v2_status (DFY_V2_CALL *listen)(dfy_v2_instance* instance, dfy_v2_error* error);
    // 请求插件结束原子会话与游戏工作，关闭状态另行查询。
    dfy_v2_status (DFY_V2_CALL *stop)(dfy_v2_instance* instance, dfy_v2_error* error);
    // 查询实际推进状态；文本借用限本次能力调用。
    dfy_v2_status (DFY_V2_CALL *status)(dfy_v2_instance* instance, dfy_v2_partner_status* output, dfy_v2_error* error);
} dfy_v2_companion_partner_api;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
