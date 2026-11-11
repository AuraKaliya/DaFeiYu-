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
#define DFY_V2_UNSUPPORTED UINT32_C(5)

// 查询限同一宿主线程；不创建实例或产生业务副作用，异常不得跨越 C ABI。
#ifdef __cplusplus
extern "C" {
#endif
#pragma pack(push, 8)
typedef uint32_t dfy_v2_status;
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
