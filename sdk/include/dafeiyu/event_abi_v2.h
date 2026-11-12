#pragma once
#include <dafeiyu/plugin_abi_v2.h>
#define DFY_V2_READER_INTERFACE "dfy.reader"
#define DFY_V2_READER_VERSION UINT32_C(1)
#define DFY_V2_READ_EVENT UINT32_C(1)
#define DFY_V2_READ_CLOSED UINT32_C(2)
#define DFY_V2_READ_CANCELLED UINT32_C(3)
#define DFY_V2_CLOSE_DRAIN UINT32_C(1)
#define DFY_V2_CLOSE_CANCEL UINT32_C(2)
#ifdef __cplusplus
extern "C" {
#endif
#pragma pack(push, 8)
// 类型化读取结果；只有 Event 有非零事件身份，只有 Closed 有非零关闭模式。
typedef struct dfy_v2_read_result {
    uint32_t outcome;
    uint32_t close_mode;
    uint64_t event_domain;
    uint64_t event_sequence;
} dfy_v2_read_result;
typedef struct dfy_v2_reader_api {
    dfy_v2_api_header header;
    // 提交读取并返回完整请求的持有引用；失败清空输出，不内联调用消费者。
    dfy_v2_status (DFY_V2_CALL *next)(dfy_v2_handle reader, dfy_v2_handle* request, dfy_v2_error* error);
    // 读取确定结果副本；Pending 返回 BUSY，输出必需且失败全零。
    dfy_v2_status (DFY_V2_CALL *result)(dfy_v2_handle request, dfy_v2_read_result* output, dfy_v2_error* error);
    // 取得事件载荷的独立持有，继续保持完整请求及事件预算；非事件结果拒绝。
    dfy_v2_status (DFY_V2_CALL *payload)(dfy_v2_handle request, dfy_v2_handle* output, dfy_v2_error* error);
} dfy_v2_reader_api;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
