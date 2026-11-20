#pragma once
#include <dafeiyu/plugin_abi_v2.h>
#define DFY_V2_MAILBOX_INTERFACE "dfy.mailbox"
#define DFY_V2_MAILBOX_VERSION UINT32_C(1)
#define DFY_V2_MAILBOX_TEXT_INTERFACE "dfy.mailbox.text"
#define DFY_V2_MAILBOX_TEXT_VERSION UINT32_C(1)
#define DFY_V2_MAILBOX_ACCEPTED UINT32_C(1)
#define DFY_V2_MAILBOX_REPEATED UINT32_C(2)
#define DFY_V2_MAILBOX_EXPIRED UINT32_C(3)
#define DFY_V2_MAILBOX_OUT_OF_ORDER UINT32_C(4)

#ifdef __cplusplus
extern "C" {
#endif
#pragma pack(push, 8)
typedef struct dfy_v2_mailbox_pair {
    uint64_t inbound, outbound;
} dfy_v2_mailbox_pair;
typedef struct dfy_v2_mailbox_receipt {
    uint32_t state, reserved;
    uint64_t mailbox, sequence;
} dfy_v2_mailbox_receipt;
typedef struct dfy_v2_mailbox_message {
    uint64_t mailbox, sequence;
    dfy_v2_handle payload;
} dfy_v2_mailbox_message;

typedef struct dfy_v2_mailbox_api {
    dfy_v2_api_header header;
    // 在当前插件与本体之间注册两条定向路线；重复接入返回原地址。
    dfy_v2_status (DFY_V2_CALL *attach)(dfy_v2_mailbox_pair* output, dfy_v2_error* error);
    // 限当前绑定线程；分类及正文只在调用内借用，发送序号在本出站信箱内从 1 递增。
    dfy_v2_status (DFY_V2_CALL *post)(uint64_t sequence, dfy_v2_string_view category, uint32_t version,
        dfy_v2_string_view text, dfy_v2_mailbox_receipt* output, dfy_v2_error* error);
    // 非阻塞读取；空箱返回 NOT_FOUND，成功转交结构载荷引用，由消费者释放。
    dfy_v2_status (DFY_V2_CALL *poll)(dfy_v2_mailbox_message* output, dfy_v2_error* error);
} dfy_v2_mailbox_api;
typedef struct dfy_v2_mailbox_text {
    dfy_v2_string_view category, text;
    uint32_t version, reserved;
} dfy_v2_mailbox_text;
typedef struct dfy_v2_mailbox_text_api {
    dfy_v2_api_header header;
    // 从收件引用中同时读取可信分类和单文本载荷，视图借用至引用释放。
    dfy_v2_status (DFY_V2_CALL *inspect)(dfy_v2_handle payload, dfy_v2_mailbox_text* output, dfy_v2_error* error);
} dfy_v2_mailbox_text_api;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
