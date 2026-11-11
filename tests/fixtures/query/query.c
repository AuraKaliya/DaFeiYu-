#include <dafeiyu/plugin_abi.h>
#include <string.h>

static uint32_t mode, queries, lifecycle_calls;

// 选择下一次查询返回的故障并重置计数，仅供测试控制。
DFY_EXPORT void DFY_CALL dfy_fixture_mode(uint32_t selected) { mode = selected; queries = lifecycle_calls = 0; }
// 返回查询次数，验证拒绝发生在加载前检查还是实际查询之后。
DFY_EXPORT uint32_t DFY_CALL dfy_fixture_queries(void) { return queries; }
// 返回实例入口调用次数，确保查询流程没有启动生命周期。
DFY_EXPORT uint32_t DFY_CALL dfy_fixture_lifecycle_calls(void) { return lifecycle_calls; }

// 记录错误的生命周期调用，不产生实例。
static dfy_status DFY_CALL create(const dfy_host_api* host, dfy_string_view config,
    dfy_plugin_instance** instance, dfy_error* error) {
    (void)host; (void)config;
    ++lifecycle_calls;
    if (instance) *instance = NULL;
    if (error) error->length = 0;
    return DFY_UNSUPPORTED;
}
// 记录启动或停止调用，夹具不实现这些操作。
static dfy_status DFY_CALL transition(dfy_plugin_instance* instance, dfy_error* error) {
    (void)instance;
    ++lifecycle_calls;
    if (error) error->length = 0;
    return DFY_UNSUPPORTED;
}
// 记录销毁调用，夹具没有实际实例资源。
static void DFY_CALL destroy(dfy_plugin_instance* instance) { (void)instance; ++lifecycle_calls; }

// 按测试模式返回状态、空表或不兼容表，所有故障均通过真实 DLL 导出交付。
// 步骤：初始化输出和正确表 → 按模式改变一个条件 → 返回选定结果。
DFY_EXPORT dfy_status DFY_CALL dfy_plugin_query(uint32_t requested, const dfy_plugin_api** output, dfy_error* error) {
    static const dfy_plugin_api valid = {sizeof(dfy_plugin_api), DFY_ABI_VERSION, DFY_ARCH_X64, 0,
        create, transition, transition, destroy};
    static dfy_plugin_api table;
    ++queries;
    if (!output) return DFY_INVALID_ARGUMENT;
    *output = NULL;
    if (error) error->length = 0;
    if (requested != DFY_ABI_VERSION) return DFY_ABI_MISMATCH;
    table = valid;

    switch (mode) {
    case 1: if (error) { memcpy(error->text, "private-test-marker", 19); error->length = 19; } return DFY_FAILED;
    case 2: return DFY_OK;
    case 3: table.struct_size = 4; break;
    case 4: table.struct_size = sizeof(table) + 8; break;
    case 5: table.abi_version = 2; break;
    case 6: table.architecture = 0; break;
    case 7: table.reserved = 1; break;
    case 8: table.create = NULL; break;
    case 9: table.start = NULL; break;
    case 10: table.stop = NULL; break;
    case 11: table.destroy = NULL; break;
    case 12: if (error) error->length = DFY_ERROR_CAPACITY + 1; return DFY_FAILED;
    case 13: if (error) { error->length = 1; error->text[0] = 'x'; } break;
    case 14: *output = (const dfy_plugin_api*)(uintptr_t)1; return DFY_FAILED;
    case 15: return UINT32_MAX;
    default: break;
    }
    *output = &table;
    return DFY_OK;
}
