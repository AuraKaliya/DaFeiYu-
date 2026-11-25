#include "api.h"
#include <stddef.h>
#include <stdlib.h>
_Static_assert(sizeof(dfy_v2_lifecycle_api) == 48 && offsetof(dfy_v2_lifecycle_api, destroy) == 40, "lifecycle layout");
struct dfy_v2_instance {
    dfy_v2_query_interface_fn host;
    const dfy_v2_reference_api* refs;
    const dfy_v2_subscription_api* subscriptions;
    dfy_v2_handle reader, request;
    uint32_t started, stopping, stopped, delay;
};
static uint32_t mode, counts[4];
// 选择一次失败或延后停止模式，仅用于真实 DLL 验收。
DFY_V2_EXPORT void DFY_V2_CALL dfy_lifecycle_mode(uint32_t value) { mode = value; }
// 返回创建、启动、停止请求及销毁计数，未知索引为零。
DFY_V2_EXPORT uint32_t DFY_V2_CALL dfy_lifecycle_count(uint32_t index) { return index < 4 ? counts[index] : 0; }
// 返回无借用文本的状态，成功或失败都初始化可选错误缓冲。
static dfy_v2_status report(dfy_v2_status status, dfy_v2_error* error) { if (error) error->length = 0; return status; }
// 分配真实 C 实例，仅保存宿主查询函数，不保存配置借用。
static dfy_v2_status DFY_V2_CALL create(dfy_v2_query_interface_fn host, dfy_v2_string_view config,
    dfy_v2_instance** output, dfy_v2_error* error) {
    if (output) *output = 0;
    if (!output || !host || !config.data || !config.length) return report(DFY_V2_INVALID_ARGUMENT, error);
    if (mode == 1) { mode = 0; return report(DFY_V2_FAILED, error); }
    *output = (dfy_v2_instance*)calloc(1, sizeof(**output));
    if (!*output) return report(DFY_V2_RESOURCE_EXHAUSTED, error);
    (*output)->host = host; ++counts[0];
    return report(DFY_V2_OK, error);
}
// 查询精确服务表，错误不返回不兼容入口。
static const void* service(dfy_v2_instance* instance, const char* name, uint64_t length, uint32_t size) {
    const dfy_v2_string_view id = {name, length};
    const void* table = 0;
    const dfy_v2_api_header* header;
    if (instance->host(id, 1, &table, 0)) return 0;
    header = (const dfy_v2_api_header*)table;
    return header && header->struct_size == size && header->interface_version == 1 ? table : 0;
}
// 建立真实订阅和等待，使部分启动失败确有宿主资源需要清理。
// 步骤：查询服务 → 接纳订阅和读取 → 按模式报告启动结果。
static dfy_v2_status DFY_V2_CALL start(dfy_v2_instance* instance, dfy_v2_error* error) {
    const dfy_v2_reader_api* reads;
    dfy_v2_status status;
    if (!instance || instance->started || instance->stopping) return report(DFY_V2_BUSY, error);
    instance->refs = (const dfy_v2_reference_api*)service(instance, DFY_V2_REFERENCE_INTERFACE, sizeof(DFY_V2_REFERENCE_INTERFACE)-1, sizeof(dfy_v2_reference_api));
    instance->subscriptions = (const dfy_v2_subscription_api*)service(instance, DFY_V2_SUBSCRIPTION_INTERFACE, sizeof(DFY_V2_SUBSCRIPTION_INTERFACE)-1, sizeof(dfy_v2_subscription_api));
    reads = (const dfy_v2_reader_api*)service(instance, DFY_V2_READER_INTERFACE, sizeof(DFY_V2_READER_INTERFACE)-1, sizeof(dfy_v2_reader_api));
    if (!instance->refs || !instance->subscriptions || !reads || !instance->refs->release ||
        !instance->subscriptions->subscribe || !instance->subscriptions->close || !reads->next) return report(DFY_V2_UNSUPPORTED, error);
    status = instance->subscriptions->subscribe(&instance->reader, error); if (status) return status;
    status = reads->next(instance->reader, &instance->request, error); if (status) return status;
    instance->started = 1; ++counts[1];
    if (mode == 2) { mode = 0; return report(DFY_V2_FAILED, error); }
    return report(DFY_V2_OK, error);
}
// 接受停止并归还已有句柄，失败重试不会重复释放已完成部分。
// 步骤：处理故障及重复 → 关闭订阅 → 逐项释放持有 → 标记停止已受理。
static dfy_v2_status DFY_V2_CALL request_stop(dfy_v2_instance* instance, dfy_v2_error* error) {
    const dfy_v2_handle empty = {0};
    dfy_v2_status status;
    if (!instance) return report(DFY_V2_INVALID_ARGUMENT, error);
    if (instance->stopping) return report(DFY_V2_OK, error);
    ++counts[2];
    if (mode == 3) { mode = 0; return report(DFY_V2_FAILED, error); }
    if (mode == 5) return report(DFY_V2_FAILED, error);
    if (instance->reader.id) {
        status = instance->subscriptions->close(instance->reader, DFY_V2_CLOSE_CANCEL, error); if (status) return status;
        status = instance->refs->release(instance->reader, error); if (status) return status;
        instance->reader = empty;
    }
    if (instance->request.id) {
        status = instance->refs->release(instance->request, error); if (status) return status;
        instance->request = empty;
    }
    instance->delay = mode == 4 ? 1 : (mode == 6 ? 4 : 0); mode = 0;
    instance->stopping = 1;
    return report(DFY_V2_OK, error);
}
// 用有界推进区分已受理与真实停止，终态之后不重新变为 Pending。
static dfy_v2_status DFY_V2_CALL poll_stop(dfy_v2_instance* instance, uint32_t* output, dfy_v2_error* error) {
    if (output) *output = 0;
    if (!instance || !output) return report(DFY_V2_INVALID_ARGUMENT, error);
    if (!instance->stopping) return report(DFY_V2_BUSY, error);
    if (instance->delay) { --instance->delay; *output = DFY_V2_STOP_PENDING; }
    else { instance->stopped = 1; *output = DFY_V2_STOPPED; }
    return report(DFY_V2_OK, error);
}
// 由插件释放已经停止的实例，调用方必须保证只调用一次。
static void DFY_V2_CALL destroy(dfy_v2_instance* instance) { if (instance) { ++counts[3]; free(instance); } }
const dfy_v2_lifecycle_api dfy_test_lifecycle = {{sizeof(dfy_v2_lifecycle_api), 1}, create, start, request_stop, poll_stop, destroy};
