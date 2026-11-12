#pragma once
#include <dafeiyu/lifecycle_abi_v2.h>
#define DFY_OBSERVER_STATS_INTERFACE "example.observer.stats"
#ifdef __cplusplus
extern "C" {
#endif
#pragma pack(push, 8)
// 模块内所有实例的累计只读统计；计数饱和，事件身份由宿主签发。
typedef struct dfy_observer_stats {
    uint64_t created, destroyed, events, fields, bytes, cancelled, closed, failures;
    uint64_t last_domain, last_sequence, last_uint64;
    int64_t last_int64;
    double last_float64;
    uint32_t kinds, last_bool;
} dfy_observer_stats;
typedef struct dfy_observer_stats_api {
    dfy_v2_api_header header;
    // 在持有模块及活动保护的宿主线程复制统计，输出必需，error 可空。
    dfy_v2_status (DFY_V2_CALL *read)(dfy_observer_stats* output, dfy_v2_error* error);
} dfy_observer_stats_api;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
