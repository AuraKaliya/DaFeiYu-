#include <dafeiyu/tool_abi_v2.h>
#include <stdio.h>
#include <string.h>
typedef struct job { uint64_t sequence; uint32_t alive, increment, ready, succeeded, effect, exited, cancel; char output[32]; } job;
struct dfy_v2_instance { uint32_t alive, running; uint64_t owner, next_job, value; dfy_v2_query_interface_fn host; job jobs[2]; };
static struct dfy_v2_instance instances[8];
static uint64_t next_owner = 0;
static uint32_t mode = 0, starts = 0, effects = 0, releases = 0;
// 返回状态并清空可选错误文本，夹具不回显输入内容。
static dfy_v2_status report(dfy_v2_status status, dfy_v2_error* error) { if (error) error->length = 0; return status; }
// 构造静态 UTF-8 字节视图，调用者按接口约定复制。
static dfy_v2_string_view view(const char* text) { dfy_v2_string_view result = {text,(uint64_t)strlen(text)}; return result; }
// 比较有界接口名，不依赖输入含有终止符。
static int equal(dfy_v2_string_view value, const char* text) { return value.data && value.length == strlen(text) && memcmp(value.data,text,(size_t)value.length) == 0; }
// 从固定实例池核对指针，不解引用不属于本夹具的地址。
static struct dfy_v2_instance* owned(dfy_v2_instance* candidate) {
    unsigned n; for (n = 0; n < 8; ++n) if (candidate == &instances[n] && instances[n].alive) return &instances[n];
    return NULL;
}
// 确认活动实例能在当前绑定中查询宿主服务。
static int active(struct dfy_v2_instance* instance) {
    const void* api = NULL;
    return instance && instance->running && instance->host(view(DFY_V2_REFERENCE_INTERFACE),1,&api,NULL) == DFY_V2_OK && api;
}
// 校验无参数工具的空对象，接受 JSON 空白但不接受其他字段。
static int empty_object(dfy_v2_string_view value) {
    uint64_t n; unsigned state = 0;
    if (!value.data || value.length > 65536) return 0;
    for (n = 0; n < value.length; ++n) {
        const char c = value.data[n]; if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (!state && c == '{') state = 1; else if (state == 1 && c == '}') state = 2; else return 0;
    }
    return state == 2;
}
// 按实例所有者和不复用序号查找工作，失效票据不指向新工作。
static job* find_job(struct dfy_v2_instance* instance, dfy_v2_tool_ticket ticket) {
    unsigned n; if (!active(instance) || ticket.owner != instance->owner || !ticket.sequence) return NULL;
    for (n = 0; n < 2; ++n) if (instance->jobs[n].alive && instance->jobs[n].sequence == ticket.sequence) return &instance->jobs[n];
    return NULL;
}
// 从固定池创建实例，完整准备后才交付指针。
// 步骤：清空输出并校验参数 → 取得空槽 → 固定实例代次与宿主入口。
static dfy_v2_status DFY_V2_CALL create(dfy_v2_query_interface_fn host, dfy_v2_string_view config, dfy_v2_instance** output, dfy_v2_error* error) {
    unsigned n; if (output) *output = NULL;
    if (!output || !host || !config.data || next_owner == UINT64_MAX) return report(DFY_V2_INVALID_ARGUMENT,error);
    for (n = 0; n < 8; ++n) if (!instances[n].alive) {
        memset(&instances[n],0,sizeof(instances[n])); instances[n].alive = 1; instances[n].owner = ++next_owner;
        instances[n].host = host; *output = &instances[n]; return report(DFY_V2_OK,error);
    }
    return report(DFY_V2_RESOURCE_EXHAUSTED,error);
}
// 启动有效实例，不执行工具。
static dfy_v2_status DFY_V2_CALL start(dfy_v2_instance* raw, dfy_v2_error* error) {
    struct dfy_v2_instance* instance = owned(raw); if (!instance) return report(DFY_V2_INVALID_ARGUMENT,error);
    instance->running = 1; return report(DFY_V2_OK,error);
}
// 停止夹具的协作工作；没有后台线程，未执行工作不产生新效果。
static dfy_v2_status DFY_V2_CALL stop(dfy_v2_instance* raw, dfy_v2_error* error) {
    struct dfy_v2_instance* instance = owned(raw); if (!instance) return report(DFY_V2_INVALID_ARGUMENT,error);
    instance->running = 0; memset(instance->jobs,0,sizeof(instance->jobs)); return report(DFY_V2_OK,error);
}
// 只读报告实例已停止，失败清空输出。
static dfy_v2_status DFY_V2_CALL stopped(dfy_v2_instance* raw, uint32_t* state, dfy_v2_error* error) {
    struct dfy_v2_instance* instance = owned(raw); if (state) *state = 0;
    if (!instance || !state) return report(DFY_V2_INVALID_ARGUMENT,error);
    *state = instance->running ? DFY_V2_STOP_PENDING : DFY_V2_STOPPED; return report(DFY_V2_OK,error);
}
// 仅销毁已停止实例，旧票据的所有者代次不再匹配后续创建。
static void DFY_V2_CALL destroy(dfy_v2_instance* raw) { struct dfy_v2_instance* instance = owned(raw); if (instance && !instance->running) instance->alive = 0; }
// 只读返回两个实际工具，故障模式额外给出一个非法描述。
static dfy_v2_status DFY_V2_CALL count(dfy_v2_instance* raw, uint32_t* output, dfy_v2_error* error) {
    if (output) *output = 0; if (!output || !active(owned(raw))) return report(DFY_V2_INVALID_CONTEXT,error);
    *output = mode == 4 ? 3 : 2; return report(DFY_V2_OK,error);
}
// 返回稳定工具描述，不把测试状态改变混入元数据查询。
// 步骤：核对输出布局及上下文 → 清空并填写描述 → 返回静态借用视图。
static dfy_v2_status DFY_V2_CALL describe(dfy_v2_instance* raw, uint32_t index, dfy_v2_tool_description* output, dfy_v2_error* error) {
    if (!output) return report(DFY_V2_INVALID_ARGUMENT,error);
    if (output->struct_size != sizeof(*output)) { output->struct_size = 0; return report(DFY_V2_ABI_MISMATCH,error); }
    memset(output,0,sizeof(*output));
    if (!active(owned(raw)) || index >= (mode == 4 ? 3u : 2u)) return report(DFY_V2_NOT_FOUND,error);
    output->struct_size = sizeof(*output); output->version = 1; output->flags = index == 1 ? DFY_V2_TOOL_CHANGES_STATE : 0;
    output->id = view(index == 0 ? "read" : index == 1 ? "increment" : "broken"); output->description = view("fixture state tool");
    output->parameters = view(index == 2 ? "{}" : "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}");
    return report(DFY_V2_OK,error);
}
// 有界接纳一次工作，实际效果留到推进阶段。
// 步骤：清空票据并校验调用 → 查找空工作槽 → 固定序号和动作 → 交付所有者票据。
static dfy_v2_status DFY_V2_CALL begin(dfy_v2_instance* raw, dfy_v2_string_view id, const dfy_v2_tool_call* call,
    dfy_v2_tool_ticket* ticket, dfy_v2_error* error) {
    struct dfy_v2_instance* instance = owned(raw); unsigned n; if (ticket) memset(ticket,0,sizeof(*ticket));
    if (!ticket || !active(instance) || !call || call->struct_size != sizeof(*call)) return report(DFY_V2_INVALID_ARGUMENT,error);
    if (call->tool_version != 1) return report(DFY_V2_ABI_MISMATCH,error);
    if ((!equal(id,"read") && !equal(id,"increment")) || !call->session || !call->run || !call->step || !call->attempt ||
        !call->invocation || !call->call_id.data || !call->call_id.length || call->call_id.length > 128 || !empty_object(call->arguments))
        return report(DFY_V2_INVALID_ARGUMENT,error);
    if (instance->next_job == UINT64_MAX) return report(DFY_V2_RESOURCE_EXHAUSTED,error);
    for (n = 0; n < 2; ++n) if (!instance->jobs[n].alive) {
        job* work = &instance->jobs[n]; memset(work,0,sizeof(*work)); work->alive = 1; work->sequence = ++instance->next_job;
        work->increment = equal(id,"increment"); ticket->owner = instance->owner; ticket->sequence = work->sequence; ++starts;
        return report(DFY_V2_OK,error);
    }
    return report(DFY_V2_RESOURCE_EXHAUSTED,error);
}
// 首次推进形成唯一结果，后续推进只确认实际退出。
// 步骤：核对票据与输出布局 → 按取消或动作形成结果 → 延后退出 → 返回借用输出。
static dfy_v2_status DFY_V2_CALL poll(dfy_v2_instance* raw, dfy_v2_tool_ticket ticket, dfy_v2_tool_progress* output, dfy_v2_error* error) {
    struct dfy_v2_instance* instance = owned(raw); job* work = find_job(instance,ticket);
    if (!output) return report(DFY_V2_INVALID_ARGUMENT,error);
    if (output->struct_size != sizeof(*output)) { output->struct_size = 0; return report(DFY_V2_ABI_MISMATCH,error); }
    memset(output,0,sizeof(*output)); if (!work) return report(DFY_V2_INVALID_HANDLE,error);
    if (mode == 2) { mode = 0; return report(DFY_V2_FAILED,error); }
    if (!work->ready) {
        work->ready = 1; work->succeeded = !work->cancel; work->effect = DFY_V2_TOOL_NOT_APPLIED;
        if (!work->cancel && work->increment) {
            if (instance->value == UINT64_MAX) work->succeeded = 0;
            else { ++instance->value; ++effects; work->effect = DFY_V2_TOOL_APPLIED; }
        }
        (void)snprintf(work->output,sizeof(work->output),"%llu",(unsigned long long)instance->value);
        // 模拟工作已经派发、提供方却无法确认实效的终态回执。
        if (mode == 7 && work->increment) { work->effect = DFY_V2_TOOL_UNKNOWN; work->succeeded = 0; mode = 0; }
        if (work->cancel) work->exited = 1;
    } else work->exited = 1;
    if (mode == 6) { work->exited = 1; mode = 1; }
    output->struct_size = sizeof(*output); output->result_ready = 1; output->succeeded = work->succeeded;
    output->effect = work->effect; output->exited = work->exited; output->output = view(work->output);
    if (mode == 5) { mode = 0; --output->struct_size; }
    return report(DFY_V2_OK,error);
}
// 只登记首次协作取消，不回滚已经发生的效果。
static dfy_v2_status DFY_V2_CALL cancel(dfy_v2_instance* raw, dfy_v2_tool_ticket ticket, uint32_t reason, dfy_v2_error* error) {
    job* work = find_job(owned(raw),ticket); if (!work) return report(DFY_V2_INVALID_HANDLE,error);
    if (reason < DFY_V2_CANCEL_USER || reason > DFY_V2_CANCEL_DEADLINE) return report(DFY_V2_INVALID_ARGUMENT,error);
    if (!work->cancel) work->cancel = reason; return report(DFY_V2_OK,error);
}
// 仅释放已经退出的工作；模拟失败时保留原票据供安全重试。
static dfy_v2_status DFY_V2_CALL release(dfy_v2_instance* raw, dfy_v2_tool_ticket ticket, dfy_v2_error* error) {
    job* work = find_job(owned(raw),ticket); if (!work) return report(DFY_V2_INVALID_HANDLE,error);
    if (!work->exited) return report(DFY_V2_BUSY,error);
    if (mode == 1) { mode = 0; return report(DFY_V2_FAILED,error); }
    work->alive = 0; ++releases; return report(DFY_V2_OK,error);
}
static const dfy_v2_lifecycle_api lifecycle = {{sizeof(dfy_v2_lifecycle_api),1},create,start,stop,stopped,destroy};
static const dfy_v2_tool_api tools = {{sizeof(dfy_v2_tool_api),1},count,describe,begin,poll,cancel,release};
// 查询精确版本的静态能力，不创建实例或执行工作。
static dfy_v2_status DFY_V2_CALL query(dfy_v2_string_view name, uint32_t version, const void** output, dfy_v2_error* error) {
    if (!output) return report(DFY_V2_INVALID_ARGUMENT,error); *output = NULL;
    if (version != 1) return report(DFY_V2_UNSUPPORTED,error);
    if (equal(name,DFY_V2_LIFECYCLE_INTERFACE)) *output = &lifecycle;
    else if (equal(name,DFY_V2_TOOL_INTERFACE)) *output = &tools;
    return report(*output ? DFY_V2_OK : DFY_V2_UNSUPPORTED,error);
}
static const dfy_v2_plugin_api plugin = {sizeof(dfy_v2_plugin_api),DFY_V2_ABI_VERSION,DFY_V2_ARCH_X64,0,query};
// 返回唯一 v2 引导表，失败清空输出。
DFY_V2_EXPORT dfy_v2_status DFY_V2_CALL dfy_plugin_query_v2(uint32_t version, const dfy_v2_plugin_api** output, dfy_v2_error* error) {
    if (!output) return report(DFY_V2_INVALID_ARGUMENT,error); *output = NULL;
    if (version != 2) return report(DFY_V2_ABI_MISMATCH,error); *output = &plugin; return report(DFY_V2_OK,error);
}
// 设置仅供离线验收使用的一次性失败或非法描述模式。
DFY_V2_EXPORT void DFY_V2_CALL dfy_tool_test_mode(uint32_t value) { mode = value; }
// 查询接纳、实际效果或释放计数，不推进工作。
DFY_V2_EXPORT uint32_t DFY_V2_CALL dfy_tool_test_count(uint32_t index) { return index == 0 ? starts : index == 1 ? effects : releases; }
