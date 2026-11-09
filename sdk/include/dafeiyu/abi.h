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
typedef struct dfy_bytes { const char* data; uint64_t size; } dfy_bytes;
typedef struct dfy_sink { void* user; int32_t (DFY_CALL *emit)(void*, dfy_bytes); } dfy_sink;
typedef struct dfy_host_api {
    uint32_t struct_size;
    uint32_t abi_version;
    void* user;
    dfy_bytes data_directory;
    dfy_bytes instance_id;
    /* Registration: create/start only, on the plugin owner thread. */
    int32_t (DFY_CALL *register_item)(void*, dfy_bytes kind, dfy_bytes json);
    /* Thread safe, copies bytes. OK means queued; durable operations send receipts. */
    int32_t (DFY_CALL *post)(void*, dfy_bytes json);
    int32_t (DFY_CALL *is_cancelled)(void*, uint64_t call_id);
} dfy_host_api;
typedef struct dfy_plugin_api {
    uint32_t struct_size;
    uint32_t abi_version;
    const char* plugin_id;
    const char* plugin_version;
    int32_t (DFY_CALL *create)(const dfy_host_api*, dfy_bytes, void**, dfy_sink);
    int32_t (DFY_CALL *start)(void*, dfy_sink);
    int32_t (DFY_CALL *on_event)(void*, dfy_bytes, dfy_sink);
    void (DFY_CALL *quiesce)(void*);
    void (DFY_CALL *stop)(void*);
    void (DFY_CALL *destroy)(void*);
} dfy_plugin_api;
typedef int32_t (DFY_CALL *dfy_query_fn)(uint32_t, dfy_plugin_api*);
#ifdef __cplusplus
}
#endif
#endif
