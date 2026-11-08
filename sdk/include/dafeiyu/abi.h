#ifndef DAFEIYU_ABI_H
#define DAFEIYU_ABI_H
#include <stdint.h>

#if defined(_WIN32)
#define DFY_CALL __cdecl
#define DFY_EXPORT __declspec(dllexport)
#else
#define DFY_CALL
#define DFY_EXPORT __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif

#define DFY_ABI_VERSION 1u
#define DFY_OK 0
#define DFY_ERROR 1
#define DFY_CANCELLED 2

/* UTF-8, length-delimited; borrowed only for the duration of the call. */
typedef struct dfy_bytes { const char* data; uint64_t size; } dfy_bytes;
typedef struct dfy_sink {
    void* user;
    int32_t (DFY_CALL *emit)(void* user, dfy_bytes json);
} dfy_sink;
typedef struct dfy_call {
    uint32_t struct_size;
    dfy_bytes service;
    dfy_bytes method;
    dfy_bytes input_json;
    dfy_bytes context_json;
    void* cancellation_user;
    int32_t (DFY_CALL *is_cancelled)(void* user);
} dfy_call;
typedef struct dfy_host_api {
    uint32_t struct_size;
    uint32_t abi_version;
    void* user;
    /* Stable until destroy; each plugin gets its own directory. */
    dfy_bytes data_directory;
    dfy_bytes instance_id;
    /* Only callable during create. Registrations are owned by this plugin instance. */
    int32_t (DFY_CALL *register_service)(void* user, dfy_bytes service_id);
} dfy_host_api;
typedef struct dfy_plugin_api {
    uint32_t struct_size;
    uint32_t abi_version;
    const char* plugin_id;
    const char* plugin_version;
    int32_t (DFY_CALL *create)(const dfy_host_api* host, dfy_bytes config, void** instance, dfy_sink errors);
    int32_t (DFY_CALL *start)(void* instance, dfy_sink errors);
    /* Synchronous in ABI v1; callbacks may stream, but must finish before returning. */
    int32_t (DFY_CALL *invoke)(void* instance, const dfy_call* call, dfy_sink output);
    void (DFY_CALL *stop)(void* instance);
    void (DFY_CALL *destroy)(void* instance);
} dfy_plugin_api;
typedef int32_t (DFY_CALL *dfy_query_fn)(uint32_t requested_abi, dfy_plugin_api* api);

#ifdef __cplusplus
}
#endif
#endif
