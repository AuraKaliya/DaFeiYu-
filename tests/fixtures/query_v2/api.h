#pragma once
#include <dafeiyu/plugin_abi_v2.h>
#define DFY_TEST_QUERY_ID "test.query"

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
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
