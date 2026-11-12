#pragma once
#include <dafeiyu/plugin_abi_v2.h>
#include <dafeiyu/data_abi_v2.h>
#include <dafeiyu/event_abi_v2.h>
#define DFY_TEST_QUERY_ID "test.query"
#define DFY_TEST_BORROW_ID "test.borrow"
#define DFY_TEST_OPERATION_ID "test.operation"
#define DFY_TEST_CANCEL_ID "test.cancel"
#define DFY_TEST_NOTIFY_ID "test.notification"
#define DFY_TEST_HOST_ID "test.host"
#define DFY_TEST_TYPES_ID "test.host_types"
#define DFY_TEST_VALUES_ID "test.host_values"
#define DFY_TEST_READS_ID "test.host_reads"

#ifdef __cplusplus
extern "C" {
#endif
#pragma pack(push, 8)
// 仅供查询夹具使用的能力，不属于公开 SDK 的业务能力集合。
typedef struct dfy_test_query_api {
    dfy_v2_api_header header;
    // 在持有 DLL 引用的宿主线程返回固定测试值，不创建资源。
    uint32_t (DFY_V2_CALL *value)(void);
} dfy_test_query_api;
// 仅供借用验收使用的宿主数据访问函数；输出视图借用至传入引用被释放。
typedef dfy_v2_status (DFY_V2_CALL *dfy_test_view_fn)(dfy_v2_handle source, dfy_v2_bytes_view* view);
typedef struct dfy_test_borrow_api {
    dfy_v2_api_header header;
    // 调用期间借用服务表和 source，读取后取得独立引用并在成功时转交调用方。
    dfy_v2_status (DFY_V2_CALL *preserve)(const dfy_v2_reference_api* refs, dfy_test_view_fn view,
        dfy_v2_handle source, dfy_v2_handle* owned, dfy_v2_error* error);
} dfy_test_borrow_api;
extern const dfy_test_borrow_api dfy_test_borrow;
// 仅用于从纯 C DLL 消费宿主操作快照，不发布生产操作服务。
typedef struct dfy_test_operation_api {
    dfy_v2_api_header header;
    // 调用内借用宿主表和操作引用；校验服务布局后读取，输出快照归调用方持有。
    dfy_v2_status (DFY_V2_CALL *read)(const dfy_v2_operation_state_api* states,
        dfy_v2_handle operation, dfy_v2_operation_snapshot* snapshot, dfy_v2_error* error);
} dfy_test_operation_api;
extern const dfy_test_operation_api dfy_test_operation;
// 纯 C 取消消费夹具，仅在调用期间借用服务与操作引用。
typedef struct dfy_test_cancel_api {
    dfy_v2_api_header header;
    // 检查宿主表后请求取消，失败清空回执，不保留服务地址。
    dfy_v2_status (DFY_V2_CALL *request)(const dfy_v2_operation_cancel_api* cancels,
        dfy_v2_handle operation, uint32_t reason, uint32_t* disposition, dfy_v2_error* error);
    // 检查宿主表后读取取消快照，失败清空输出，不改变操作。
    dfy_v2_status (DFY_V2_CALL *read)(const dfy_v2_operation_cancel_api* cancels,
        dfy_v2_handle operation, dfy_v2_cancel_snapshot* snapshot, dfy_v2_error* error);
} dfy_test_cancel_api;
extern const dfy_test_cancel_api dfy_test_cancel;
// 由测试宿主持有并覆盖整个回调期，C DLL 只在回调内访问。
typedef struct dfy_test_notify_context {
    const dfy_v2_operation_notify_api* notifications;
    const dfy_v2_reference_api* references;
    struct dfy_test_notify_context* child;
    dfy_v2_handle registration;
    dfy_v2_handle retained;
    uint32_t calls, revoke_status, retain_status, spawn_status;
    uint32_t* sequence;
    uint32_t observed_order;
    // 可选宿主探针仅在 C 回调原生栈内执行，测试上下文须覆盖调用。
    void (DFY_V2_CALL *probe)(void*);
    void* probe_user;
} dfy_test_notify_context;
typedef struct dfy_test_notify_api {
    dfy_v2_api_header header;
    // 在宿主调度的回调内测试自撤销、操作保留及可选的新登记；不保存 DLL 全局状态。
    dfy_v2_completion_fn callback;
} dfy_test_notify_api;
extern const dfy_test_notify_api dfy_test_notify;
typedef struct dfy_test_host_api {
    dfy_v2_api_header header;
    // 在宿主绑定期间查询引用服务并返回独立持有，不保存上下文地址。
    dfy_v2_status (DFY_V2_CALL *preserve)(dfy_v2_query_interface_fn query, dfy_v2_handle source,
        dfy_v2_handle* output, dfy_v2_error* error);
} dfy_test_host_api;
extern const dfy_test_host_api dfy_test_host;
typedef struct dfy_test_types_api {
    dfy_v2_api_header header;
    // 不预编译具体事件名称或字段列表，枚举宿主描述并汇总种类位图。
    dfy_v2_status (DFY_V2_CALL *inspect)(dfy_v2_query_interface_fn query, dfy_v2_handle type,
        uint32_t* kinds, dfy_v2_error* error);
} dfy_test_types_api;
extern const dfy_test_types_api dfy_test_types;
typedef struct dfy_test_values_api {
    dfy_v2_api_header header;
    // 反射读取六种测试值并释放全部中间引用，不预编译结构名称或字段顺序。
    dfy_v2_status (DFY_V2_CALL *inspect)(dfy_v2_query_interface_fn query, dfy_v2_handle structure, dfy_v2_error* error);
} dfy_test_values_api;
extern const dfy_test_values_api dfy_test_values;
typedef struct dfy_test_reads_api {
    dfy_v2_api_header header;
    // 在当前宿主绑定中提交读取，不保存宿主表或裸上下文。
    dfy_v2_status (DFY_V2_CALL *next)(dfy_v2_query_interface_fn query, dfy_v2_handle reader, dfy_v2_handle* request, dfy_v2_error* error);
    // 在当前绑定中读取类型化结果，不自动取得数据持有。
    dfy_v2_status (DFY_V2_CALL *result)(dfy_v2_query_interface_fn query, dfy_v2_handle request, dfy_v2_read_result* result, dfy_v2_error* error);
} dfy_test_reads_api;
extern const dfy_test_reads_api dfy_test_reads;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
