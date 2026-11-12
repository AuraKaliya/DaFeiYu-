#include "observer_api.h"
#include <dafeiyu/data_abi_v2.h>
#include <dafeiyu/event_abi_v2.h>
#include <stdlib.h>
#include <string.h>

struct dfy_v2_instance {
    const dfy_v2_reference_api* refs;
    const dfy_v2_subscription_api* subscriptions;
    const dfy_v2_reader_api* reads;
    const dfy_v2_operation_notify_api* notifications;
    const dfy_v2_operation_cancel_api* cancels;
    const dfy_v2_type_api* types;
    const dfy_v2_struct_api* structures;
    const dfy_v2_value_api* values;
    dfy_v2_handle reader, request, notification;
    uint32_t started, stopping, in_callback;
};
static dfy_observer_stats statistics;

// 增加饱和计数，避免长期运行后统计回绕。
static void add(uint64_t* counter, uint64_t amount) { *counter = UINT64_MAX - *counter < amount ? UINT64_MAX : *counter + amount; }
// 初始化可选诊断，不把插件内部文本或数据作为错误返回。
static dfy_v2_status report(dfy_v2_status status, dfy_v2_error* error) { if (error) error->length = 0; return status; }
// 判断无配置字段的示例是否收到空 JSON 对象，允许 JSON 空白。
// 步骤：跳过空白 → 匹配两个括号 → 拒绝其余内容。
static int empty_config(dfy_v2_string_view text) {
    uint64_t index = 0;
    const char expected[2] = {'{', '}'};
    unsigned part;
    if (!text.data) return 0;
    for (part = 0; part < 3; ++part) {
        while (index < text.length && (text.data[index] == ' ' || text.data[index] == '\t' || text.data[index] == '\r' || text.data[index] == '\n')) ++index;
        if (part < 2 && (index == text.length || text.data[index++] != expected[part])) return 0;
    }
    return index == text.length;
}
// 查询并核对精确版本的宿主表，失败不读取或保存返回指针。
static const void* service(dfy_v2_query_interface_fn query, const char* name, uint32_t size) {
    const dfy_v2_string_view id = {name, (uint64_t)strlen(name)};
    const void* table = 0;
    const dfy_v2_api_header* header;
    if (query(id, 1, &table, 0)) return 0;
    header = (const dfy_v2_api_header*)table;
    return header && header->struct_size == size && header->interface_version == 1 ? table : 0;
}
// 释放一份插件持有，仅成功时清空本地标识，允许停止重试。
static dfy_v2_status release(dfy_v2_instance* instance, dfy_v2_handle* handle) {
    dfy_v2_status status = instance->refs->release(*handle, 0);
    if (!status) { const dfy_v2_handle empty = {0}; *handle = empty; }
    return status;
}
// 仅按宿主报告的基础种类读取统计，不预编译事件名或字段布局。
static dfy_v2_status inspect_value(const dfy_v2_value_view* value) {
    if (value->kind < DFY_V2_VALUE_BOOL || value->kind > DFY_V2_VALUE_BYTES || value->reserved) return DFY_V2_UNSUPPORTED;
    statistics.kinds |= UINT32_C(1) << value->kind;
    switch (value->kind) {
    case DFY_V2_VALUE_BOOL: if (value->data.boolean > 1) return DFY_V2_FAILED; statistics.last_bool = value->data.boolean; break;
    case DFY_V2_VALUE_INT64: statistics.last_int64 = value->data.int64; break;
    case DFY_V2_VALUE_UINT64: statistics.last_uint64 = value->data.uint64; break;
    case DFY_V2_VALUE_FLOAT64: statistics.last_float64 = value->data.float64; break;
    case DFY_V2_VALUE_STRING: if (value->data.string.length && !value->data.string.data) return DFY_V2_FAILED; add(&statistics.bytes, value->data.string.length); break;
    case DFY_V2_VALUE_BYTES: if (value->data.bytes.length && !value->data.bytes.data) return DFY_V2_FAILED; add(&statistics.bytes, value->data.bytes.length); break;
    }
    return DFY_V2_OK;
}
// 反射读取完整事件并归还中间引用，观察失败不会回写生产者载荷。
// 步骤：取得载荷及描述 → 枚举并读取字段 → 归还所有持有 → 记录事件身份。
static dfy_v2_status inspect(dfy_v2_instance* instance, dfy_v2_handle request, const dfy_v2_read_result* result) {
    dfy_v2_handle payload = {0}, type = {0};
    dfy_v2_type_info info = {0};
    dfy_v2_status status = instance->reads->payload(request, &payload, 0);
    uint64_t index;
    if (status) return status;
    status = instance->structures->descriptor(payload, &type, 0);
    if (!status) status = instance->types->describe(type, &info, 0);
    if (!status && (!info.name.data || !info.name.length || !info.contract_version || info.reserved)) status = DFY_V2_FAILED;
    for (index = 0; !status && index < info.field_count; ++index) {
        dfy_v2_handle field = {0};
        dfy_v2_field_info description = {0};
        dfy_v2_value_view value = {0};
        status = instance->types->field(type, index, &description, 0);
        if (!status) status = instance->structures->field(payload, index, &field, 0);
        if (!status) status = instance->values->read(field, &value, 0);
        if (!status) status = value.kind == description.kind ? inspect_value(&value) : DFY_V2_FAILED;
        if (release(instance, &field) && !status) status = DFY_V2_FAILED;
        if (!status) add(&statistics.fields, 1);
    }
    if (release(instance, &type) && !status) status = DFY_V2_FAILED;
    if (release(instance, &payload) && !status) status = DFY_V2_FAILED;
    if (!status) { add(&statistics.events, 1); statistics.last_domain = result->event_domain; statistics.last_sequence = result->event_sequence; }
    return status;
}
// 预声明一次读取登记，完成回调会按条件继续下一次等待。
static dfy_v2_status arm(dfy_v2_instance* instance);
// 消费一次确定结果，引用与当前借用分别处理，失败关闭自身订阅以免持续背压其他接收方。
// 步骤：释放本次通知持有 → 读取结果及事件 → 归还请求 → 按结果继续或关闭。
static void DFY_V2_CALL completed(void* user, dfy_v2_handle borrowed) {
    dfy_v2_instance* instance = (dfy_v2_instance*)user;
    dfy_v2_read_result result = {0};
    dfy_v2_status status;
    instance->in_callback = 1;
    status = release(instance, &instance->notification);
    if (!status) status = instance->reads->result(borrowed, &result, 0);
    if (!status && (result.outcome < DFY_V2_READ_EVENT || result.outcome > DFY_V2_READ_CANCELLED)) status = DFY_V2_FAILED;
    if (!status && result.outcome == DFY_V2_READ_EVENT) status = inspect(instance, borrowed, &result);
    if (!status && result.outcome == DFY_V2_READ_CANCELLED) add(&statistics.cancelled, 1);
    if (!status && result.outcome == DFY_V2_READ_CLOSED) add(&statistics.closed, 1);
    if (release(instance, &instance->request) && !status) status = DFY_V2_FAILED;
    if (!status && !instance->stopping && result.outcome != DFY_V2_READ_CLOSED) status = arm(instance);
    if (status) {
        add(&statistics.failures, 1);
        if (!instance->subscriptions->close(instance->reader, DFY_V2_CLOSE_CANCEL, 0) && !release(instance, &instance->request)) {
            release(instance, &instance->reader);
        }
    }
    instance->in_callback = 0;
}
// 接受一项读取并登记延后通知，失败保留已取得的句柄供停止收尾。
// 步骤：提交读取 → 登记一次性完成通知。
static dfy_v2_status arm(dfy_v2_instance* instance) {
    dfy_v2_status status = instance->reads->next(instance->reader, &instance->request, 0);
    return status ? status : instance->notifications->subscribe(instance->request, completed, instance, &instance->notification, 0);
}
// 分配实例并核对所有实际使用的宿主能力，不在 create 中订阅或保存配置借用。
// 步骤：校验输入 → 分配实例 → 查询并核对能力 → 提交实例或回收。
static dfy_v2_status DFY_V2_CALL create(dfy_v2_query_interface_fn host, dfy_v2_string_view config,
    dfy_v2_instance** output, dfy_v2_error* error) {
    dfy_v2_instance* instance;
    if (output) *output = 0;
    if (!output || !host || !empty_config(config)) return report(DFY_V2_INVALID_ARGUMENT, error);
    instance = (dfy_v2_instance*)calloc(1, sizeof(*instance));
    if (!instance) return report(DFY_V2_RESOURCE_EXHAUSTED, error);
    instance->refs = (const dfy_v2_reference_api*)service(host, DFY_V2_REFERENCE_INTERFACE, sizeof(dfy_v2_reference_api));
    instance->subscriptions = (const dfy_v2_subscription_api*)service(host, DFY_V2_SUBSCRIPTION_INTERFACE, sizeof(dfy_v2_subscription_api));
    instance->reads = (const dfy_v2_reader_api*)service(host, DFY_V2_READER_INTERFACE, sizeof(dfy_v2_reader_api));
    instance->notifications = (const dfy_v2_operation_notify_api*)service(host, DFY_V2_OPERATION_NOTIFY_INTERFACE, sizeof(dfy_v2_operation_notify_api));
    instance->cancels = (const dfy_v2_operation_cancel_api*)service(host, DFY_V2_OPERATION_CANCEL_INTERFACE, sizeof(dfy_v2_operation_cancel_api));
    instance->types = (const dfy_v2_type_api*)service(host, DFY_V2_TYPE_INTERFACE, sizeof(dfy_v2_type_api));
    instance->structures = (const dfy_v2_struct_api*)service(host, DFY_V2_STRUCT_INTERFACE, sizeof(dfy_v2_struct_api));
    instance->values = (const dfy_v2_value_api*)service(host, DFY_V2_VALUE_INTERFACE, sizeof(dfy_v2_value_api));
    if (!instance->refs || !instance->subscriptions || !instance->reads || !instance->notifications || !instance->cancels ||
        !instance->types || !instance->structures || !instance->values || !instance->refs->retain || !instance->refs->release ||
        !instance->subscriptions->subscribe || !instance->subscriptions->close || !instance->subscriptions->read || !instance->reads->next || !instance->reads->result ||
        !instance->reads->payload || !instance->notifications->subscribe || !instance->notifications->revoke || !instance->notifications->read ||
        !instance->cancels->request || !instance->cancels->read ||
        !instance->types->describe || !instance->types->field || !instance->structures->descriptor || !instance->structures->field || !instance->values->read) {
        free(instance); return report(DFY_V2_UNSUPPORTED, error);
    }
    *output = instance; add(&statistics.created, 1);
    return report(DFY_V2_OK, error);
}
// 启动独立接收方及一次等待，重复启动或停止后的启动均拒绝。
// 步骤：核对启动状态 → 创建订阅 → 接受并观察读取。
static dfy_v2_status DFY_V2_CALL start(dfy_v2_instance* instance, dfy_v2_error* error) {
    dfy_v2_status status;
    if (!instance || instance->started || instance->stopping) return report(DFY_V2_BUSY, error);
    instance->started = 1;
    status = instance->subscriptions->subscribe(&instance->reader, 0);
    if (!status) status = arm(instance);
    return report(status, error);
}
// 取消等待并关闭订阅，已成功释放的句柄不在停止重试中重放。
// 步骤：拒绝活动回调 → 请求取消并撤销通知 → 关闭队列并读取取消结果 → 释放持有。
static dfy_v2_status DFY_V2_CALL request_stop(dfy_v2_instance* instance, dfy_v2_error* error) {
    dfy_v2_status status;
    uint32_t disposition = 0;
    int cancelled = 0;
    if (!instance) return report(DFY_V2_INVALID_ARGUMENT, error);
    if (instance->in_callback) return report(DFY_V2_BUSY, error);
    instance->stopping = 1;
    if (instance->request.id) { status = instance->cancels->request(instance->request, DFY_V2_CANCEL_OWNER_STOP, &disposition, 0); if (status) return report(status, error); }
    if (instance->notification.id) { status = instance->notifications->revoke(instance->notification, 0); if (status) return report(status, error); }
    if (instance->reader.id) { status = instance->subscriptions->close(instance->reader, DFY_V2_CLOSE_CANCEL, 0); if (status) return report(status, error); }
    if (instance->request.id) {
        dfy_v2_read_result result = {0};
        cancelled = !instance->reads->result(instance->request, &result, 0) && result.outcome == DFY_V2_READ_CANCELLED;
    }
    status = release(instance, &instance->notification); if (status) return report(status, error);
    status = release(instance, &instance->request); if (status) return report(status, error);
    if (cancelled) add(&statistics.cancelled, 1);
    return report(release(instance, &instance->reader), error);
}
// 在全部自身持有已释放且回调退出后报告停止完成，不把请求受理当作完成。
static dfy_v2_status DFY_V2_CALL poll_stop(dfy_v2_instance* instance, uint32_t* state, dfy_v2_error* error) {
    if (state) *state = 0;
    if (!instance || !state) return report(DFY_V2_INVALID_ARGUMENT, error);
    if (!instance->stopping) return report(DFY_V2_BUSY, error);
    *state = instance->in_callback || instance->reader.id || instance->request.id || instance->notification.id ? DFY_V2_STOP_PENDING : DFY_V2_STOPPED;
    return report(DFY_V2_OK, error);
}
// 仅由宿主在确认停止后释放实例，私有内存不跨模块回收。
static void DFY_V2_CALL destroy(dfy_v2_instance* instance) { if (instance) { add(&statistics.destroyed, 1); free(instance); } }
// 复制模块统计，不返回实例或载荷地址。
static dfy_v2_status DFY_V2_CALL read_stats(dfy_observer_stats* output, dfy_v2_error* error) {
    if (!output) return report(DFY_V2_INVALID_ARGUMENT, error);
    *output = statistics;
    return report(DFY_V2_OK, error);
}
static const dfy_v2_lifecycle_api lifecycle = {{sizeof(dfy_v2_lifecycle_api), 1}, create, start, request_stop, poll_stop, destroy};
static const dfy_observer_stats_api stats = {{sizeof(dfy_observer_stats_api), 1}, read_stats};
// 精确提供两个已实现的静态能力，查询不创建实例。
static dfy_v2_status DFY_V2_CALL query_interface(dfy_v2_string_view name, uint32_t version, const void** output, dfy_v2_error* error) {
    if (output) *output = 0;
    if (!output || !name.data || !name.length || !version) return report(DFY_V2_INVALID_ARGUMENT, error);
    if (version == 1 && name.length == sizeof(DFY_V2_LIFECYCLE_INTERFACE)-1 && !memcmp(name.data, DFY_V2_LIFECYCLE_INTERFACE, name.length)) *output = &lifecycle;
    if (version == 1 && name.length == sizeof(DFY_OBSERVER_STATS_INTERFACE)-1 && !memcmp(name.data, DFY_OBSERVER_STATS_INTERFACE, name.length)) *output = &stats;
    return report(*output ? DFY_V2_OK : DFY_V2_UNSUPPORTED, error);
}
static const dfy_v2_plugin_api bootstrap = {sizeof(dfy_v2_plugin_api), 2, DFY_V2_ARCH_X64, 0, query_interface};
// 返回固定 v2 引导表，失败输出清空且不产生业务副作用。
DFY_V2_EXPORT dfy_v2_status DFY_V2_CALL dfy_plugin_query_v2(uint32_t version, const dfy_v2_plugin_api** output, dfy_v2_error* error) {
    if (output) *output = 0;
    if (!output) return report(DFY_V2_INVALID_ARGUMENT, error);
    if (version != 2) return report(DFY_V2_ABI_MISMATCH, error);
    *output = &bootstrap;
    return report(DFY_V2_OK, error);
}
