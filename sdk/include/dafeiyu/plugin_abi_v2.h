#pragma once
#include <stdint.h>

#if !defined(_WIN32) || !defined(_WIN64) || (!defined(_M_X64) && !defined(__x86_64__))
#error "DaFeiYu ABI v2 requires Windows x64"
#endif
#define DFY_V2_CALL __cdecl
#ifdef DFY_PLUGIN_BUILD
#define DFY_V2_EXPORT __declspec(dllexport)
#else
#define DFY_V2_EXPORT
#endif
#define DFY_V2_ABI_VERSION UINT32_C(2)
#define DFY_V2_ARCH_X64 UINT32_C(1)
#define DFY_V2_QUERY_EXPORT "dfy_plugin_query_v2"
#define DFY_V2_ERROR_CAPACITY 256
#define DFY_V2_OK UINT32_C(0)
#define DFY_V2_INVALID_ARGUMENT UINT32_C(1)
#define DFY_V2_ABI_MISMATCH UINT32_C(2)
#define DFY_V2_BUSY UINT32_C(3)
#define DFY_V2_UNSUPPORTED UINT32_C(5)
#define DFY_V2_INVALID_HANDLE UINT32_C(6)
#define DFY_V2_RESOURCE_EXHAUSTED UINT32_C(7)
#define DFY_V2_REFERENCE_INTERFACE "dfy.references"
#define DFY_V2_REFERENCE_VERSION UINT32_C(1)
#define DFY_V2_OPERATION_STATE_INTERFACE "dfy.operation.state"
#define DFY_V2_OPERATION_STATE_VERSION UINT32_C(1)
#define DFY_V2_OPERATION_PENDING UINT32_C(1)
#define DFY_V2_OPERATION_SUCCEEDED UINT32_C(2)
#define DFY_V2_OPERATION_FAILED UINT32_C(3)
#define DFY_V2_OPERATION_CANCELLED UINT32_C(4)
#define DFY_V2_OPERATION_CANCEL_INTERFACE "dfy.operation.cancel"
#define DFY_V2_OPERATION_CANCEL_VERSION UINT32_C(1)
#define DFY_V2_CANCEL_USER UINT32_C(1)
#define DFY_V2_CANCEL_OWNER_STOP UINT32_C(2)
#define DFY_V2_CANCEL_DEADLINE UINT32_C(3)
#define DFY_V2_CANCEL_RECORDED UINT32_C(1)
#define DFY_V2_CANCEL_ALREADY_REQUESTED UINT32_C(2)
#define DFY_V2_CANCEL_ALREADY_SETTLED UINT32_C(3)
#define DFY_V2_OPERATION_NOTIFY_INTERFACE "dfy.operation.notify"
#define DFY_V2_OPERATION_NOTIFY_VERSION UINT32_C(1)
#define DFY_V2_NOTIFICATION_PENDING UINT32_C(1)
#define DFY_V2_NOTIFICATION_RUNNING UINT32_C(2)
#define DFY_V2_NOTIFICATION_DELIVERED UINT32_C(3)
#define DFY_V2_NOTIFICATION_REVOKED UINT32_C(4)

// 查询限同一宿主线程；不创建实例或产生业务副作用，异常不得跨越 C ABI。
#ifdef __cplusplus
extern "C" {
#endif
#pragma pack(push, 8)
typedef uint32_t dfy_v2_status;
// 不透明引用标识；三项全零为空，其余由签发方解释，数值复制不增加持有引用。
typedef struct dfy_v2_handle {
    uint64_t domain;
    uint64_t id;
    uint64_t generation;
} dfy_v2_handle;
// 只读字节视图不持有数据；调用借用或持有引用借用的有效期由返回它的接口声明。
typedef struct dfy_v2_bytes_view {
    const uint8_t* data;
    uint64_t length;
} dfy_v2_bytes_view;
// UTF-8 字节视图仅在调用期间借用；长度不包含终止符，非零长度要求有效内存。
typedef struct dfy_v2_string_view {
    const char* data;
    uint64_t length;
} dfy_v2_string_view;
// 调用方持有错误缓冲；成功时 length 为零，失败可写入不超过容量的完整 UTF-8 文本。
typedef struct dfy_v2_error {
    uint32_t length;
    char text[DFY_V2_ERROR_CAPACITY];
} dfy_v2_error;
// 每个能力表以此成员开头；大小是完整表的字节数，版本仅标识该能力的精确布局。
typedef struct dfy_v2_api_header {
    uint32_t struct_size;
    uint32_t interface_version;
} dfy_v2_api_header;

// 宿主引用服务在同一宿主线程调用，表及函数地址须覆盖全部引用的使用与释放。
typedef struct dfy_v2_reference_api {
    dfy_v2_api_header header;
    // 从有效借用或持有引用取得独立持有句柄；输出必需且失败全零，error 可空。
    dfy_v2_status (DFY_V2_CALL *retain)(dfy_v2_handle source, dfy_v2_handle* owned, dfy_v2_error* error);
    // 仅释放持有引用；空句柄成功，过期引用拒绝，失败不消耗引用，error 可空。
    // 不表示业务完成、取消或异步清理完成；调用方成功后自行清空其句柄副本。
    dfy_v2_status (DFY_V2_CALL *release)(dfy_v2_handle owned, dfy_v2_error* error);
} dfy_v2_reference_api;

// 调用方持有的状态副本；零值无效，终态不再改变，不表示关联资源已回收。
typedef struct dfy_v2_operation_snapshot {
    uint32_t state;
} dfy_v2_operation_snapshot;
// 宿主状态服务限同一宿主线程；操作引用可独立保留，释放不取消操作。
typedef struct dfy_v2_operation_state_api {
    dfy_v2_api_header header;
    // 读取有效操作引用的快照，不等待、推进操作或调用消费者；输出必需且失败全零。
    // 查询成功返回 OK，包括操作失败或已取消；error 可空且只描述本次查询错误。
    dfy_v2_status (DFY_V2_CALL *read)(dfy_v2_handle operation,
        dfy_v2_operation_snapshot* snapshot, dfy_v2_error* error);
} dfy_v2_operation_state_api;

// 首个有效取消原因的调用方副本；reason 为零表示尚未记录请求，与操作终态独立。
typedef struct dfy_v2_cancel_snapshot {
    uint32_t reason;
} dfy_v2_cancel_snapshot;
// 同一宿主线程的可选操作取消服务；不撤销其他等待者，不同步执行消费者或清理。
typedef struct dfy_v2_operation_cancel_api {
    dfy_v2_api_header header;
    // 请求协作取消，输出必需且失败清零；成功回执区分新登记、已有请求及已结算。
    // reason 仅接受三种已定义原因；首个原因保留，成功不表示已取消或工作已经退出。
    dfy_v2_status (DFY_V2_CALL *request)(dfy_v2_handle operation, uint32_t reason,
        uint32_t* disposition, dfy_v2_error* error);
    // 只读首个取消原因；输出必需且失败全零，未请求时成功返回零，error 可空。
    dfy_v2_status (DFY_V2_CALL *read)(dfy_v2_handle operation,
        dfy_v2_cancel_snapshot* snapshot, dfy_v2_error* error);
} dfy_v2_operation_cancel_api;

// 同一宿主线程的延后唤醒；operation 仅在本次回调内借用，user 由登记方持有。
// 回调不得跨 ABI 抛异常；宿主保持操作和回调代码有效至调用返回。
typedef void (DFY_V2_CALL *dfy_v2_completion_fn)(void* user, dfy_v2_handle operation);
// 通知登记的调用方状态副本；Delivered/Revoked 表示该登记已无活动或将来回调。
typedef struct dfy_v2_notification_snapshot {
    uint32_t state;
} dfy_v2_notification_snapshot;
// 通知服务限同一宿主线程；每个登记独立且不可 retain，释放引用本身不撤销通知。
typedef struct dfy_v2_operation_notify_api {
    dfy_v2_api_header header;
    // 为有效操作登记一次性唤醒，输出独立登记引用；callback 必需，user 可空。
    // 输出必需且失败全零；即使已有终态也不在本调用内执行回调，error 可空。
    dfy_v2_status (DFY_V2_CALL *subscribe)(dfy_v2_handle operation, dfy_v2_completion_fn callback,
        void* user, dfy_v2_handle* registration, dfy_v2_error* error);
    // 读取有效登记引用的状态副本；输出必需且失败全零，不推进回调。
    dfy_v2_status (DFY_V2_CALL *read)(dfy_v2_handle registration,
        dfy_v2_notification_snapshot* snapshot, dfy_v2_error* error);
    // 撤销未开始的回调；活动回调返回 BUSY 且不改状态，已交付或已撤销时幂等成功。
    // 成功后该登记不会进入或仍处于回调，不取消底层操作；error 可空。
    dfy_v2_status (DFY_V2_CALL *revoke)(dfy_v2_handle registration, dfy_v2_error* error);
} dfy_v2_operation_notify_api;

// 按非空 UTF-8 名称和非零版本查询静态能力表；未知名称或版本返回 UNSUPPORTED。
// 输出必需且失败清空，error 可为空；成功表只读并借用至模块引用释放，不得由调用方释放。
typedef dfy_v2_status (DFY_V2_CALL *dfy_v2_query_interface_fn)(
    dfy_v2_string_view name, uint32_t interface_version, const void** api, dfy_v2_error* error);
typedef struct dfy_v2_plugin_api {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t architecture;
    uint32_t reserved;
    // 必需查询函数；只能返回已实现的能力，不声明实例生命周期或宿主服务已可用。
    dfy_v2_query_interface_fn query_interface;
} dfy_v2_plugin_api;

// 查询 v2 静态引导表；输出必需且失败清空，成功表借用至模块释放，error 可为空。
typedef dfy_v2_status (DFY_V2_CALL *dfy_v2_query_fn)(
    uint32_t requested_abi, const dfy_v2_plugin_api** api, dfy_v2_error* error);
// 使用独立固定导出查询 v2，引导表布局和 v1 查询签名互不替换。
DFY_V2_EXPORT dfy_v2_status DFY_V2_CALL dfy_plugin_query_v2(
    uint32_t requested_abi, const dfy_v2_plugin_api** api, dfy_v2_error* error);
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
