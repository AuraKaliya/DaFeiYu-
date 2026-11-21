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
#define DFY_V2_TRANSITION_HOOK_INTERFACE "dfy.hook.transition"
#define DFY_V2_TRANSITION_HOOK_VERSION UINT32_C(1)
#define DFY_V2_TRANSITION_EVENT_CAPTURED UINT32_C(1)
#define DFY_V2_TRANSITION_EVENT_DECODED UINT32_C(2)
#define DFY_V2_TRANSITION_EVENT_BEFORE_DISPATCH UINT32_C(3)
#define DFY_V2_TRANSITION_EVENT_DISPATCHED UINT32_C(4)
#define DFY_V2_TRANSITION_MAIL_CONTEXT UINT32_C(5)
#define DFY_V2_TRANSITION_ASK_PRESENTED UINT32_C(6)
#define DFY_V2_TRANSITION_ASK_ANSWERED UINT32_C(7)
#define DFY_V2_TRANSITION_ASK_ENDED UINT32_C(8)
#define DFY_V2_TRANSITION_REPLY_ENQUEUED UINT32_C(9)
#define DFY_V2_TRANSITION_REPLY_ACKNOWLEDGED UINT32_C(10)
#define DFY_V2_TRANSITION_RUN_EXITED UINT32_C(11)
#define DFY_V2_TRANSITION_CAN_DENY UINT32_C(1)
#define DFY_V2_TRANSITION_CAN_REWRITE UINT32_C(2)
// ASK 状态与回合结果在各自的阶段内使用下列稳定值。
#define DFY_V2_TRANSITION_ASK_WAITING UINT32_C(1)
#define DFY_V2_TRANSITION_ASK_HAS_ANSWER UINT32_C(2)
#define DFY_V2_TRANSITION_ASK_EXPIRED UINT32_C(3)
#define DFY_V2_TRANSITION_ASK_CANCELLED UINT32_C(4)
#define DFY_V2_TRANSITION_ASK_FAILED UINT32_C(5)
#define DFY_V2_TRANSITION_RUN_SUCCEEDED UINT32_C(1)
#define DFY_V2_TRANSITION_RUN_FAILED UINT32_C(2)
#define DFY_V2_TRANSITION_RUN_CANCELLED UINT32_C(3)

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

typedef struct dfy_v2_transition_hook_input {
    uint32_t point, permissions, status, reserved;
    uint64_t owner, domain, sequence, ask, mailbox, message;
    dfy_v2_string_view category, value;
} dfy_v2_transition_hook_input;
typedef struct dfy_v2_transition_hook_api {
    dfy_v2_api_header header;
    // 启动后按登记顺序枚举；作用域以事件发送方或回合所有者为准。
    dfy_v2_status (DFY_V2_CALL *describe)(dfy_v2_instance*, uint32_t index, dfy_v2_hook_description*, dfy_v2_error*);
    // 在宿主线程同步执行；只可按 permissions 拒绝或改写借用字段，written 全一表示不改写。
    dfy_v2_status (DFY_V2_CALL *apply)(dfy_v2_instance*, uint32_t index, const dfy_v2_transition_hook_input*,
        char* output, uint64_t capacity, uint32_t* verdict, uint64_t* written, dfy_v2_error*);
} dfy_v2_transition_hook_api;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
