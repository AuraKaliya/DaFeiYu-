#pragma once
#include <dafeiyu/plugin_abi_v2.h>

#define DFY_V2_AGENT_INTERFACE "dfy.agent"
#define DFY_V2_AGENT_VERSION UINT32_C(1)
#define DFY_V2_AGENT_PERSONA_VERSION UINT32_C(2)
#define DFY_V2_AGENT_IDLE UINT32_C(1)
#define DFY_V2_AGENT_QUEUED UINT32_C(2)
#define DFY_V2_AGENT_RUNNING UINT32_C(3)
#define DFY_V2_AGENT_SUCCEEDED UINT32_C(4)
#define DFY_V2_AGENT_FAILED UINT32_C(5)
#define DFY_V2_AGENT_CANCELLED UINT32_C(6)
#define DFY_V2_AGENT_ALLOW_CHANGES UINT32_C(1)
#define DFY_V2_AGENT_REPLACE_HISTORY UINT32_C(2)
// 只在第 2 版服务请求把本体人格用于面向玩家的回合。
#define DFY_V2_AGENT_PLAYER_FACING UINT32_C(4)

#ifdef __cplusplus
extern "C" {
#endif
#pragma pack(push, 8)
typedef struct dfy_v2_agent_input {
    dfy_v2_string_view prompt;
    dfy_v2_string_view instructions;
    dfy_v2_string_view binding;
    const dfy_v2_string_view* tools;
    uint32_t tool_count;
    uint32_t flags;
} dfy_v2_agent_input;
typedef struct dfy_v2_agent_status {
    uint64_t request;
    uint32_t phase;
    uint32_t reserved;
    dfy_v2_string_view answer;
} dfy_v2_agent_status;
typedef struct dfy_v2_agent_api {
    dfy_v2_api_header header;
    // 在启动或活动回调中创建独立上下文，宿主绑定调用实例。
    dfy_v2_status (DFY_V2_CALL *open)(uint64_t* session, dfy_v2_error* error);
    // 复制输入并排队，工具必须属于原插件实例；返回前不分派模型或工具。
    dfy_v2_status (DFY_V2_CALL *submit)(uint64_t session, const dfy_v2_agent_input* input, uint64_t* request, dfy_v2_error* error);
    // 查询原会话最新请求；答案只借用至本次插件回调返回或同会话修改，以较早者为准。
    dfy_v2_status (DFY_V2_CALL *status)(uint64_t session, dfy_v2_agent_status* status, dfy_v2_error* error);
    // 取消原请求；过期或已结算请求返回 NOT_FOUND，取消后仍需等待实际退出。
    dfy_v2_status (DFY_V2_CALL *cancel)(uint64_t session, uint64_t request, dfy_v2_error* error);
    // 请求关闭整个上下文，宿主推进退出；同实例忙碌时卸载返回 BUSY。
    dfy_v2_status (DFY_V2_CALL *close)(uint64_t session, dfy_v2_error* error);
} dfy_v2_agent_api;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
