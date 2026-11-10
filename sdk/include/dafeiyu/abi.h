#ifndef DAFEIYU_ABI_H
#define DAFEIYU_ABI_H
#include <stdint.h>
#ifdef _WIN32
#define DFY_CALL __cdecl
#define DFY_EXPORT __declspec(dllexport)
#else
#define DFY_CALL
#define DFY_EXPORT __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif
#define DFY_ABI_VERSION 2u
#define DFY_OK 0
#define DFY_ERROR 1
#define DFY_BUSY 2
#define DFY_CLOSED 3

typedef struct dfy_bytes {
    const char* data;
    uint64_t size;
} dfy_bytes;

typedef struct dfy_sink {
    void* user;
    // 将字节结果交给接收方，并返回 ABI 接收状态。
    int32_t(DFY_CALL* emit)(void*, dfy_bytes);
} dfy_sink;

typedef struct dfy_host_api {
    uint32_t struct_size;
    uint32_t abi_version;
    void* user;
    dfy_bytes data_directory;
    dfy_bytes instance_id;

    // 仅在插件所属线程的创建或启动阶段登记服务和动作。
    int32_t(DFY_CALL* register_item)(void*, dfy_bytes kind, dfy_bytes json);

    // 线程安全地复制并入队消息；成功仅表示入队，持久化操作另有回执。
    int32_t(DFY_CALL* post)(void*, dfy_bytes json);
    // 查询调用是否已失效、被取消或超过期限。
    int32_t(DFY_CALL* is_cancelled)(void*, uint64_t call_id);
} dfy_host_api;

typedef struct dfy_plugin_api {
    uint32_t struct_size;
    uint32_t abi_version;
    const char* plugin_id;
    const char* plugin_version;
    // 在插件所属线程根据宿主接口和配置创建插件对象，并报告创建错误。
    int32_t(DFY_CALL* create)(const dfy_host_api*, dfy_bytes, void**, dfy_sink);
    // 启动插件对象并完成初始服务注册。
    int32_t(DFY_CALL* start)(void*, dfy_sink);
    // 在插件所属线程接收宿主事件，调用结果通过宿主消息接口回传。
    int32_t(DFY_CALL* on_event)(void*, dfy_bytes, dfy_sink);
    // 通知插件停止接收新工作并开始结束在途活动。
    void(DFY_CALL* quiesce)(void*);
    // 停止插件活动并等待其自有工作结束。
    void(DFY_CALL* stop)(void*);
    // 在停止完成后销毁插件对象。
    void(DFY_CALL* destroy)(void*);
} dfy_plugin_api;

// 定义宿主按 ABI 版本查询插件接口表的导出函数类型。
typedef int32_t(DFY_CALL* dfy_query_fn)(uint32_t, dfy_plugin_api*);
#ifdef __cplusplus
}
#endif
#endif
