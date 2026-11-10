#include <stddef.h>
#include <string.h>
#pragma pack(push, 1)
#include <dafeiyu/plugin_abi.h>
struct packing_probe { char tag; uint64_t value; };
_Static_assert(sizeof(struct packing_probe) == 9, "SDK must restore caller packing");
#pragma pack(pop)
_Static_assert(sizeof(dfy_string_view) == 16 && _Alignof(dfy_string_view) == 8, "string layout");
_Static_assert(sizeof(dfy_error) == 260 && _Alignof(dfy_error) == 4, "error layout");
_Static_assert(sizeof(dfy_event) == 24 && offsetof(dfy_event, turn_id) == 16, "event layout");
_Static_assert(sizeof(dfy_host_api) == 40 && offsetof(dfy_host_api, subscribe) == 16, "host layout");
_Static_assert(sizeof(dfy_plugin_api) == 48 && offsetof(dfy_plugin_api, destroy) == 40, "plugin layout");

// 将样例固定诊断复制到调用方缓冲；超长文本省略，不截断 UTF-8 字符。
static dfy_status report(dfy_status status, dfy_error* error, const char* message) {
    if (error) {
        const size_t length = strlen(message);
        error->length = length <= DFY_ERROR_CAPACITY ? (uint32_t)length : 0;
        memcpy(error->text, message, error->length);
    }
    return status;
}

// 查询样例不创建实例；校验基本调用参数后明确返回不支持。
// 步骤：清空输出句柄 → 检查必需参数 → 返回查询专用诊断。
static dfy_status DFY_CALL create(const dfy_host_api* host, dfy_string_view config,
    dfy_plugin_instance** instance, dfy_error* error) {
    if (!instance) {
        return report(DFY_INVALID_ARGUMENT, error, "instance output required");
    }
    *instance = NULL;
    if (!host || !config.data || !config.length) {
        return report(DFY_INVALID_ARGUMENT, error, "host and configuration required");
    }
    return report(DFY_UNSUPPORTED, error, "query-only example has no instances");
}

// 查询样例没有可启动或停止的实例，生命周期入口仅占位声明契约。
static dfy_status DFY_CALL unavailable(dfy_plugin_instance* instance, dfy_error* error) {
    (void)instance;
    return report(DFY_UNSUPPORTED, error, "query-only example has no instances");
}

// 查询样例从不分配实例，因此没有实例资源需要释放。
static void DFY_CALL destroy(dfy_plugin_instance* instance) { (void)instance; }

static const dfy_plugin_api api = {sizeof(dfy_plugin_api), DFY_ABI_VERSION, DFY_ARCH_X64, 0,
    create, unavailable, unavailable, destroy};

// 返回模块存活期间有效的静态表，不进行业务初始化或注册。
// 步骤：清空输出 → 核对请求版本 → 返回借用接口表。
DFY_EXPORT dfy_status DFY_CALL dfy_plugin_query(uint32_t requested_abi, const dfy_plugin_api** output, dfy_error* error) {
    if (error) {
        error->length = 0;
    }
    if (!output) {
        return report(DFY_INVALID_ARGUMENT, error, "API output required");
    }
    *output = NULL;
    if (requested_abi != DFY_ABI_VERSION) {
        return report(DFY_ABI_MISMATCH, error, "unsupported ABI version");
    }
    *output = &api;
    return DFY_OK;
}
