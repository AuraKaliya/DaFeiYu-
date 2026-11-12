#include "api.h"

// 从当前宿主查询生产引用服务并保留来源，不保留宿主表或借用句柄。
// 步骤：清空输出 → 查询并核对引用表 → 取得独立持有。
static dfy_v2_status DFY_V2_CALL preserve(dfy_v2_query_interface_fn query, dfy_v2_handle source,
    dfy_v2_handle* output, dfy_v2_error* error) {
    const void* table = 0;
    const dfy_v2_reference_api* references;
    dfy_v2_status status;
    const dfy_v2_string_view name = {DFY_V2_REFERENCE_INTERFACE, sizeof(DFY_V2_REFERENCE_INTERFACE) - 1};
    if (output) { const dfy_v2_handle empty = {0}; *output = empty; }
    if (error) error->length = 0;
    if (!query || !output) return DFY_V2_INVALID_ARGUMENT;
    status = query(name, 1, &table, error);
    if (status != DFY_V2_OK) return status;
    if (!table) return DFY_V2_FAILED;
    references = (const dfy_v2_reference_api*)table;
    if (references->header.struct_size != sizeof(*references) || references->header.interface_version != 1 ||
        !references->retain || !references->release) return DFY_V2_UNSUPPORTED;
    return references->retain(source, output, error);
}
const dfy_test_host_api dfy_test_host = {{sizeof(dfy_test_host_api), 1}, preserve};
