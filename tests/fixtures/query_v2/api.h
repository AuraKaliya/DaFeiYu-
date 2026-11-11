#pragma once
#include <dafeiyu/plugin_abi_v2.h>
#define DFY_TEST_QUERY_ID "test.query"
#define DFY_TEST_BORROW_ID "test.borrow"

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
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
