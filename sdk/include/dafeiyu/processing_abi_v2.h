#pragma once
#include <dafeiyu/event_abi_v2.h>
#define DFY_V2_PROCESSING_INTERFACE "dfy.processing"
#define DFY_V2_PROCESSING_VERSION UINT32_C(1)
#define DFY_V2_PROCESSING_WAITING UINT32_C(1)
#define DFY_V2_PROCESSING_RUNNING UINT32_C(2)
#define DFY_V2_PROCESSING_EXITED UINT32_C(3)
#ifdef __cplusplus
extern "C" {
#endif
#pragma pack(push, 8)
// 处理状态副本：result 为业务结果，exit 为实际退出，均使用 OPERATION 常量；顺序未启用时身份为零。
typedef struct dfy_v2_processing_info {
    uint32_t phase, result, exit, cancel_reason;
    uint64_t event_domain, event_sequence, order_domain, order_sequence;
} dfy_v2_processing_info;
// 所有入口仅限同宿主线程有效绑定；处理引用不可复制，释放引用不终结宿主工作。
typedef struct dfy_v2_processing_api {
    dfy_v2_api_header header;
    // 只读处理及事件身份，输出必需，失败清零；不推进授予或实际执行。
    dfy_v2_status (DFY_V2_CALL *read)(dfy_v2_handle work, dfy_v2_processing_info* output, dfy_v2_error* error);
    // 确认实际开始，未获顺序资格返回 BUSY；已经开始或结束返回 FAILED，不调用消费者。
    dfy_v2_status (DFY_V2_CALL *start)(dfy_v2_handle work, dfy_v2_error* error);
    // 固定运行中的业务终态，接受 SUCCEEDED、FAILED、CANCELLED；重复结算失败且不释放顺序占用。
    dfy_v2_status (DFY_V2_CALL *complete)(dfy_v2_handle work, uint32_t terminal, dfy_v2_error* error);
    // 消费者确认不会再执行本项处理后调用；必须已有业务终态，随后才释放占用。
    dfy_v2_status (DFY_V2_CALL *finish)(dfy_v2_handle work, dfy_v2_error* error);
    // 登记首个协作取消原因，回执必需且失败清零；运行中取消不产生业务或退出终态。
    dfy_v2_status (DFY_V2_CALL *cancel)(dfy_v2_handle work, uint32_t reason, uint32_t* disposition, dfy_v2_error* error);
    // 返回结构载荷独立引用，仍保持完整处理记录和预算；输出必需，失败清零。
    dfy_v2_status (DFY_V2_CALL *payload)(dfy_v2_handle work, dfy_v2_handle* output, dfy_v2_error* error);
} dfy_v2_processing_api;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
