#pragma once
#include <stdint.h>

#if !defined(_WIN32) || !defined(_WIN64) || (!defined(_M_X64) && !defined(__x86_64__))
#error "DaFeiYu ABI v1 requires Windows x64"
#endif
#define DFY_CALL __cdecl
#ifdef DFY_PLUGIN_BUILD
#define DFY_EXPORT __declspec(dllexport)
#else
#define DFY_EXPORT
#endif
#define DFY_ABI_VERSION UINT32_C(1)
#define DFY_ARCH_X64 UINT32_C(1)
#define DFY_QUERY_EXPORT "dfy_plugin_query"
#define DFY_ERROR_CAPACITY 256
#define DFY_OK UINT32_C(0)
#define DFY_INVALID_ARGUMENT UINT32_C(1)
#define DFY_ABI_MISMATCH UINT32_C(2)
#define DFY_BUSY UINT32_C(3)
#define DFY_NOT_FOUND UINT32_C(4)
#define DFY_UNSUPPORTED UINT32_C(5)
#define DFY_FAILED UINT32_C(6)
#define DFY_EVENT_TURN_STARTED UINT32_C(1)
#define DFY_EVENT_TURN_COMPLETED UINT32_C(2)
#define DFY_EVENT_TURN_FAILED UINT32_C(3)
#define DFY_EVENT_SESSION_CLEARED UINT32_C(4)

// 所有入口和宿主服务在同一宿主线程调用；异常不得越过边界，首版不支持插件后台线程。
#ifdef __cplusplus
extern "C" {
#endif
#pragma pack(push, 8)
typedef uint32_t dfy_status;
typedef struct dfy_host_context dfy_host_context;
typedef struct dfy_plugin_instance dfy_plugin_instance;
// 借用的 UTF-8 字节视图；非零长度要求有效指针，具体借用期由所在接口规定。
typedef struct dfy_string_view {
    const char* data;
    uint64_t length;
} dfy_string_view;
// 错误缓冲由调用方持有；length 为 UTF-8 字节数，不要求末尾零，不得超过容量。
typedef struct dfy_error {
    uint32_t length;
    char text[DFY_ERROR_CAPACITY];
} dfy_error;
typedef struct dfy_event {
    uint32_t struct_size;
    uint32_t kind;
    uint64_t session_id;
    uint64_t turn_id;
} dfy_event;

// 宿主线程同步通知；event 仅本次调用有效，失败返回状态且异常不得越过边界。
typedef dfy_status (DFY_CALL *dfy_event_callback)(void* user, const dfy_event* event, dfy_error* error);
// 每实例独立的宿主表，表地址和 context 必须稳定保留至实例销毁完成。
typedef struct dfy_host_api {
    uint32_t struct_size;
    uint32_t abi_version;
    dfy_host_context* context;
    // 为当前实例订阅全部事件；成功输出非零标识，失败输出零，回调资源保留到撤销及执行结束。
    dfy_status (DFY_CALL *subscribe)(dfy_host_context*, dfy_event_callback, void* user, uint64_t* subscription, dfy_error*);
    // 仅撤销当前实例的订阅；未知标识返回 NOT_FOUND，自取消不提前销毁当前回调资源。
    dfy_status (DFY_CALL *unsubscribe)(dfy_host_context*, uint64_t subscription, dfy_error*);
    // 尽力输出诊断，文本仅在调用期间有效；不得通过同一事件总线递归发布。
    void (DFY_CALL *diagnostic)(dfy_host_context*, dfy_string_view message);
} dfy_host_api;
typedef struct dfy_plugin_api {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t architecture;
    uint32_t reserved;
    // 创建私有实例并校验配置；配置仅在调用期间有效，失败必须清理并输出空句柄。
    dfy_status (DFY_CALL *create)(const dfy_host_api*, dfy_string_view config_json, dfy_plugin_instance**, dfy_error*);
    // 在宿主线程启动已创建实例，可注册订阅；失败后由宿主调用 stop 处理部分启动。
    dfy_status (DFY_CALL *start)(dfy_plugin_instance*, dfy_error*);
    // 停止实例，成功后允许销毁；失败保留实例和 DLL 并允许重试，成功后的重复停止应返回 OK。
    dfy_status (DFY_CALL *stop)(dfy_plugin_instance*, dfy_error*);
    // 仅在停止成功且宿主已撤销全部注册后销毁实例；分配方释放自身资源，不得抛出异常。
    void (DFY_CALL *destroy)(dfy_plugin_instance*);
} dfy_plugin_api;

// 查询静态接口表，不创建实例；失败输出空表，成功表借用至模块释放，error 可为空。
typedef dfy_status (DFY_CALL *dfy_query_fn)(uint32_t requested_abi, const dfy_plugin_api** api, dfy_error* error);
// 导出固定 C 名称的查询入口，调用约定与 dfy_query_fn 相同。
DFY_EXPORT dfy_status DFY_CALL dfy_plugin_query(uint32_t requested_abi, const dfy_plugin_api** api, dfy_error* error);
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
