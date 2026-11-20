#pragma once
#include <dafeiyu/lifecycle_abi_v2.h>
#define DFY_V2_HOOK_INTERFACE "dfy.hooks"
#define DFY_V2_HOOK_VERSION UINT32_C(1)
#define DFY_V2_HOOK_MODEL_REQUEST UINT32_C(1)
#define DFY_V2_HOOK_TOOL_START UINT32_C(2)
#define DFY_V2_HOOK_TOOL_RESULT UINT32_C(3)
#define DFY_V2_HOOK_MAIN_SESSION UINT32_C(1)
#define DFY_V2_HOOK_OWN_SESSION UINT32_C(2)
#define DFY_V2_HOOK_OTHER_SESSION UINT32_C(4)
#define DFY_V2_HOOK_ALLOW UINT32_C(1)
#define DFY_V2_HOOK_DENY UINT32_C(2)
#define DFY_V2_HOOK_CONFIRM UINT32_C(3)
#define DFY_V2_TEXT_HOOK_INTERFACE "dfy.hook.text"
#define DFY_V2_TEXT_HOOK_VERSION UINT32_C(1)
#define DFY_V2_TEXT_HOOK_INPUT UINT32_C(1)
#define DFY_V2_TEXT_HOOK_MODEL_REQUEST UINT32_C(2)
#define DFY_V2_TEXT_HOOK_MODEL_RESPONSE UINT32_C(3)
#define DFY_V2_TEXT_HOOK_TOOL_ARGUMENTS UINT32_C(4)
#define DFY_V2_TEXT_HOOK_REPLY UINT32_C(5)
#define DFY_V2_TEXT_HOOK_CHAIN UINT32_C(1)
#define DFY_V2_TEXT_HOOK_UNCHANGED UINT64_MAX

#ifdef __cplusplus
extern "C" {
#endif
#pragma pack(push, 8)
typedef struct dfy_v2_hook_description {
    uint32_t point, sessions;
} dfy_v2_hook_description;
typedef struct dfy_v2_hook_input {
    uint32_t point, reserved;
    uint64_t session, run, step, attempt, owner, goal_revision;
    dfy_v2_string_view tool_id;
} dfy_v2_hook_input;

typedef struct dfy_v2_hook_api {
    dfy_v2_api_header header;
    // 在启动后按注册顺序枚举；超出尾部返回 NOT_FOUND。
    dfy_v2_status (DFY_V2_CALL *describe)(dfy_v2_instance*, uint32_t index, dfy_v2_hook_description*, dfy_v2_error*);
    // 在原实例宿主线程同步裁定，输入及字符串仅在调用内借用；CONFIRM 仅适用于 TOOL_START。
    dfy_v2_status (DFY_V2_CALL *decide)(dfy_v2_instance*, uint32_t index, const dfy_v2_hook_input*, uint32_t* verdict, dfy_v2_error*);
} dfy_v2_hook_api;

typedef struct dfy_v2_text_hook_description {
    uint32_t point, sessions, flags;
} dfy_v2_text_hook_description;
typedef struct dfy_v2_text_hook_input {
    uint32_t point, reserved;
    uint64_t session, run, step, attempt, owner, goal_revision;
    dfy_v2_string_view field, value;
} dfy_v2_text_hook_input;
typedef struct dfy_v2_text_hook_api {
    dfy_v2_api_header header;
    // 在启动后按注册顺序枚举；超出尾部返回 NOT_FOUND 并清空输出。
    dfy_v2_status (DFY_V2_CALL *describe)(dfy_v2_instance*, uint32_t index, dfy_v2_text_hook_description*, dfy_v2_error*);
    // 字段和值仅在调用内借用；宿主提供输出缓冲，长度全一表示保持原值，零表示改为空值。
    dfy_v2_status (DFY_V2_CALL *rewrite)(dfy_v2_instance*, uint32_t index, const dfy_v2_text_hook_input*,
        char* output, uint64_t capacity, uint64_t* written, dfy_v2_error*);
} dfy_v2_text_hook_api;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
