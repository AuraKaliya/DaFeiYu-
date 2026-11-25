#include "api.h"
#include <stddef.h>
#pragma pack(push, 1)
#include <dafeiyu/processing_abi_v2.h>
struct processing_pack_probe { char tag; uint64_t value; };
#pragma pack(pop)
_Static_assert(sizeof(struct processing_pack_probe) == 9,"processing SDK restores packing");
_Static_assert(sizeof(dfy_v2_processing_info) == 48 && offsetof(dfy_v2_processing_info,order_domain) == 32,"processing snapshot layout");
_Static_assert(sizeof(dfy_v2_processing_api) == 56 && offsetof(dfy_v2_processing_api,payload) == 48,"processing table layout");

// 从独立 C DLL 分步报告业务完成与实际退出，完整校验表后才读取入口。
// 步骤：清空输出并核对参数 → 精确查询能力 → 按阶段开始及结算或确认退出 → 返回独立载荷。
static dfy_v2_status DFY_V2_CALL step(dfy_v2_query_interface_fn query, dfy_v2_handle work, uint32_t phase,
    dfy_v2_handle* payload, dfy_v2_error* error) {
    const dfy_v2_string_view name = {DFY_V2_PROCESSING_INTERFACE,sizeof(DFY_V2_PROCESSING_INTERFACE)-1};
    const void* table = 0; const dfy_v2_processing_api* api;
    dfy_v2_processing_info info = {0}; const dfy_v2_handle empty = {0}; dfy_v2_status status;
    if (payload) *payload = empty;
    if (error) error->length = 0;
    if (!query || !payload || phase > 1) return DFY_V2_INVALID_ARGUMENT;

    status = query(name,1,&table,error);
    if (status) return status;
    api = (const dfy_v2_processing_api*)table;
    if (!api || api->header.struct_size != sizeof(*api) || api->header.interface_version != 1 ||
        !api->read || !api->start || !api->complete || !api->finish || !api->cancel || !api->payload) return DFY_V2_UNSUPPORTED;
    status = api->read(work,&info,error);
    if (status) return status;
    if (phase == 1) return api->finish(work,error);

    status = api->start(work,error);
    if (!status) status = api->complete(work,DFY_V2_OPERATION_SUCCEEDED,error);
    if (!status) status = api->payload(work,payload,error);
    return status;
}
const dfy_test_processing_api dfy_test_processing = {{sizeof(dfy_test_processing_api),1},step};
