#pragma once
#include <dafeiyu/plugin_abi_v2.h>
#define DFY_V2_ROUTED_EVENT_INTERFACE "dfy.event.routes"
#define DFY_V2_ROUTED_EVENT_VERSION UINT32_C(1)
#define DFY_V2_EVENT_ASK UINT32_C(1)
#define DFY_V2_EVENT_MAIL UINT32_C(2)
#define DFY_V2_EVENT_ASK_FEEDBACK UINT32_C(3)
#define DFY_V2_EVENT_ROUTE_CAPACITY 8192

#ifdef __cplusplus
extern "C" {
#endif
#pragma pack(push, 8)
typedef struct dfy_v2_event_receipt {
    uint64_t domain, sequence;
    uint32_t repeated, reserved;
} dfy_v2_event_receipt;

// 宿主提供正文缓冲区，解析器填写路由类型、字节数和 UTF-8 JSON 对象。
typedef struct dfy_v2_event_projection {
    uint32_t kind, length;
    char json[DFY_V2_EVENT_ROUTE_CAPACITY];
} dfy_v2_event_projection;

// 只解析原事件；载荷和输出均在本次同步调用内有效，宿主随后提交实际路由。
typedef dfy_v2_status (DFY_V2_CALL *dfy_v2_event_decoder)(void* context, dfy_v2_string_view payload,
    dfy_v2_event_projection* output);
typedef struct dfy_v2_routed_event_api {
    dfy_v2_api_header header;
    // 启动时登记私有事件。schema 为闭合的字段名到 string、uint 或 bool 的映射。
    dfy_v2_status (DFY_V2_CALL *register_event)(dfy_v2_string_view type, uint32_t version, dfy_v2_string_view schema,
        dfy_v2_event_decoder decoder, void* context, dfy_v2_error* error);
    // 每实例从 1 连续提交序号。接纳只表示已入队，解析在宿主后续推进时进行。
    dfy_v2_status (DFY_V2_CALL *emit)(dfy_v2_string_view type, uint32_t version, uint64_t sequence,
        dfy_v2_string_view payload, dfy_v2_event_receipt* output, dfy_v2_error* error);
} dfy_v2_routed_event_api;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
