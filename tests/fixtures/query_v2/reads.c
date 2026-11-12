#include "api.h"
#include <stddef.h>
_Static_assert(sizeof(dfy_v2_read_result) == 24 && offsetof(dfy_v2_read_result, event_domain) == 8, "read result layout");
_Static_assert(sizeof(dfy_v2_reader_api) == 32 && offsetof(dfy_v2_reader_api, payload) == 24, "reader table layout");
// 查询并校验完整读取服务，失败不访问任何返回指针。
static dfy_v2_status service(dfy_v2_query_interface_fn query, const dfy_v2_reader_api** api, dfy_v2_error* error) {
    const dfy_v2_string_view name = {DFY_V2_READER_INTERFACE, sizeof(DFY_V2_READER_INTERFACE) - 1};
    const void* table = 0;
    dfy_v2_status status;
    if (error) error->length = 0;
    if (!query) return DFY_V2_INVALID_ARGUMENT;
    status = query(name, 1, &table, error);
    if (status) return status;
    *api = (const dfy_v2_reader_api*)table;
    return *api && (*api)->header.struct_size == sizeof(**api) && (*api)->header.interface_version == 1 &&
        (*api)->next && (*api)->result && (*api)->payload ? DFY_V2_OK : DFY_V2_UNSUPPORTED;
}
// 通过纯 C 取得一次完整请求，失败清空输出。
static dfy_v2_status DFY_V2_CALL next(dfy_v2_query_interface_fn query, dfy_v2_handle reader, dfy_v2_handle* output, dfy_v2_error* error) {
    const dfy_v2_reader_api* api = 0;
    dfy_v2_status status = service(query, &api, error);
    if (output) { const dfy_v2_handle empty = {0}; *output = empty; }
    if (!output) return DFY_V2_INVALID_ARGUMENT;
    return status ? status : api->next(reader, output, error);
}
// 通过纯 C 读取结果副本，Pending 不冒充流结束。
static dfy_v2_status DFY_V2_CALL result(dfy_v2_query_interface_fn query, dfy_v2_handle request, dfy_v2_read_result* output, dfy_v2_error* error) {
    const dfy_v2_reader_api* api = 0;
    dfy_v2_status status = service(query, &api, error);
    if (output) { const dfy_v2_read_result empty = {0}; *output = empty; }
    if (!output) return DFY_V2_INVALID_ARGUMENT;
    return status ? status : api->result(request, output, error);
}
const dfy_test_reads_api dfy_test_reads = {{sizeof(dfy_test_reads_api), 1}, next, result};
