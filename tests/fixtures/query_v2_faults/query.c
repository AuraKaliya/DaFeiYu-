#include <dafeiyu/plugin_abi_v2.h>
#include <string.h>
static uint32_t mode, queries;
static void (DFY_V2_CALL *observer)(void*);
static void* observer_user;
// 安装仅供验收的调用观察器，调用方负责上下文寿命。
DFY_V2_EXPORT void DFY_V2_CALL dfy_fixture_observer(void (DFY_V2_CALL *callback)(void*), void* user) {
    observer = callback; observer_user = user;
}
// 在能力原生栈内触发宿主观察，再返回确定结果。
static uint32_t DFY_V2_CALL value(void) { if (observer) observer(observer_user); return 42; }
typedef struct test_api { dfy_v2_api_header header; uint32_t (DFY_V2_CALL *value)(void); } test_api;

// 选择引导查询故障并清空查询计数，不创建插件实例。
DFY_V2_EXPORT void DFY_V2_CALL dfy_fixture_mode(uint32_t selected) { mode = selected; queries = 0; }
// 返回本次控制阶段发生的引导查询次数。
DFY_V2_EXPORT uint32_t DFY_V2_CALL dfy_fixture_queries(void) { return queries; }
// 按精确名称和版本返回测试能力，或制造能力前缀与返回状态故障。
// 步骤：清空输出 → 匹配测试能力 → 按模式返回错误或表 → 在原生栈内通知观察器。
static dfy_v2_status DFY_V2_CALL query_interface(dfy_v2_string_view name, uint32_t version,
    const void** output, dfy_v2_error* error) {
    static test_api table;
    const test_api valid = {{sizeof(table), 1}, value};
    if (output) *output = 0;
    if (error) error->length = 0;
    if (!output) return DFY_V2_INVALID_ARGUMENT;
    if (version != 1 || name.length != 15 || !name.data || memcmp(name.data, "test.capability", 15)) return DFY_V2_UNSUPPORTED;
    table = valid;
    switch (mode) {
    case 20: *output = (const void*)(uintptr_t)1; return DFY_V2_UNSUPPORTED;
    case 21: return DFY_V2_OK;
    case 22: table.header.struct_size = 4; break;
    case 23: table.header.struct_size += 8; break;
    case 24: table.header.interface_version = 2; break;
    case 25: if (error) error->length = DFY_V2_ERROR_CAPACITY + 1; return DFY_V2_UNSUPPORTED;
    case 26: if (error) error->length = 1; break;
    case 27: return UINT32_MAX;
    default: break;
    }
    if (observer) observer(observer_user);
    *output = &table;
    return DFY_V2_OK;
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
