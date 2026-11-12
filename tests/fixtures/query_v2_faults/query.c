#include <dafeiyu/plugin_abi_v2.h>
#include <string.h>
static uint32_t mode, queries;

// 选择引导查询故障并清空查询计数，不创建插件实例。
DFY_V2_EXPORT void DFY_V2_CALL dfy_fixture_mode(uint32_t selected) { mode = selected; queries = 0; }
// 返回本次控制阶段发生的引导查询次数。
DFY_V2_EXPORT uint32_t DFY_V2_CALL dfy_fixture_queries(void) { return queries; }
// 明确拒绝所有业务能力，夹具只用于引导表校验。
static dfy_v2_status DFY_V2_CALL query_interface(dfy_v2_string_view name, uint32_t version,
    const void** output, dfy_v2_error* error) {
    (void)name; (void)version;
    if (output) *output = 0;
    if (error) error->length = 0;
    return output ? DFY_V2_UNSUPPORTED : DFY_V2_INVALID_ARGUMENT;
}
// 通过真实导出返回单项受控故障，失败指针专用于验证宿主不解引用失败结果。
// 步骤：初始化有效表和输出 → 按模式改变一个边界 → 返回表或错误。
DFY_V2_EXPORT dfy_v2_status DFY_V2_CALL dfy_plugin_query_v2(
    uint32_t version, const dfy_v2_plugin_api** output, dfy_v2_error* error) {
    static dfy_v2_plugin_api table;
    const dfy_v2_plugin_api valid = {sizeof(table), 2, DFY_V2_ARCH_X64, 0, query_interface};
    ++queries;
    if (!output) return DFY_V2_INVALID_ARGUMENT;
    *output = 0;
    if (error) error->length = 0;
    if (version != 2) return DFY_V2_ABI_MISMATCH;
    table = valid;
    switch (mode) {
    case 1: *output = (const dfy_v2_plugin_api*)(uintptr_t)1;
        if (error) { memcpy(error->text, "private-marker", 14); error->length = 14; } return DFY_V2_UNSUPPORTED;
    case 2: return DFY_V2_OK;
    case 3: table.struct_size = 4; break;
    case 4: table.struct_size += 8; break;
    case 5: table.abi_version = 1; break;
    case 6: table.architecture = 0; break;
    case 7: table.reserved = 1; break;
    case 8: table.query_interface = 0; break;
    case 9: if (error) error->length = DFY_V2_ERROR_CAPACITY + 1; return DFY_V2_UNSUPPORTED;
    case 10: if (error) error->length = 1; break;
    case 11: return UINT32_MAX;
    default: break;
    }
    *output = &table;
    return DFY_V2_OK;
}
