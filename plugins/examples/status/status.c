#include <dafeiyu/tool_abi_v2.h>
#include <string.h>

enum action { read_mode, set_idle, set_focus };
struct dfy_v2_instance {
    uint64_t owner, sequence;
    int running, pending, ready, exited, cancelled, focus;
    enum action action;
    uint32_t effect;
};

static dfy_v2_instance instances[16];
static uint64_t next_owner;
static const char* const ids[] = {"read","set_idle","set_focus"};
static const char* const descriptions[] = {"Read the panel mode.","Set the panel mode to idle.","Set the panel mode to focus."};

// 返回 C ABI 状态，不回显调用正文。
static dfy_v2_status report(dfy_v2_status status, dfy_v2_error* error) {
    if (error) error->length = 0;
    return status;
}

// 生成借用的静态字符串视图。
static dfy_v2_string_view view(const char* text) { dfy_v2_string_view value = {text,strlen(text)}; return value; }
// 按显式长度比较标识。
static int equal(dfy_v2_string_view value, const char* text) {
    return value.data && value.length == strlen(text) && memcmp(value.data,text,(size_t)value.length) == 0;
}

// 从插件拥有的固定实例池核对活动地址。
static dfy_v2_instance* owned(dfy_v2_instance* raw) {
    for (size_t i = 0; i < sizeof(instances)/sizeof(instances[0]); ++i)
        if (raw == &instances[i] && raw->owner) return raw;
    return NULL;
}

// 仅接受 JSON 空白和一对对象括号，示例动作的目标由工具身份确定。
static int empty_object(dfy_v2_string_view input) {
    unsigned parts = 0;
    if (!input.data || input.length > 65536) return 0;
    for (uint64_t i = 0; i < input.length; ++i) {
        const char c = input.data[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        if ((parts == 0 && c == '{') || (parts == 1 && c == '}')) ++parts;
        else return 0;
    }
    return parts == 2;
}

// 从原实例代次和本次序号验证工作票据。
static dfy_v2_instance* work(dfy_v2_instance* raw, dfy_v2_tool_ticket ticket) {
    dfy_v2_instance* instance = owned(raw);
    return instance && instance->pending && instance->owner == ticket.owner && instance->sequence == ticket.sequence ? instance : NULL;
}

// 创建初始空闲的面板实例。
// 步骤：核对生命周期参数 → 找到空槽 → 分配不复用的实例标识。
static dfy_v2_status DFY_V2_CALL create(dfy_v2_query_interface_fn host, dfy_v2_string_view config, dfy_v2_instance** output, dfy_v2_error* error) {
    if (!output) return report(DFY_V2_INVALID_ARGUMENT,error);
    *output = NULL;
    if (!host || !config.data) return report(DFY_V2_INVALID_ARGUMENT,error);
    if (next_owner == UINT64_MAX) return report(DFY_V2_RESOURCE_EXHAUSTED,error);
    for (size_t i = 0; i < sizeof(instances)/sizeof(instances[0]); ++i) if (!instances[i].owner) {
        memset(&instances[i],0,sizeof(instances[i]));
        instances[i].owner = ++next_owner;
        *output = &instances[i];
        return report(DFY_V2_OK,error);
    }
    return report(DFY_V2_RESOURCE_EXHAUSTED,error);
}

// 开启面板实例的工具接纳。
static dfy_v2_status DFY_V2_CALL start(dfy_v2_instance* raw, dfy_v2_error* error) {
    dfy_v2_instance* instance = owned(raw);
    if (!instance) return report(DFY_V2_INVALID_ARGUMENT,error);
    instance->running = 1;
    return report(DFY_V2_OK,error);
}

// 等待原工作释放后停止面板实例。
static dfy_v2_status DFY_V2_CALL stop(dfy_v2_instance* raw, dfy_v2_error* error) {
    dfy_v2_instance* instance = owned(raw);
    if (!instance) return report(DFY_V2_INVALID_ARGUMENT,error);
    if (instance->pending) return report(DFY_V2_BUSY,error);
    instance->running = 0;
    return report(DFY_V2_OK,error);
}

// 仅查询实例的停止状态。
static dfy_v2_status DFY_V2_CALL stopped(dfy_v2_instance* raw, uint32_t* output, dfy_v2_error* error) {
    const dfy_v2_instance* instance = owned(raw);
    if (!output) return report(DFY_V2_INVALID_ARGUMENT,error);
    *output = instance ? (instance->running ? DFY_V2_STOP_PENDING : DFY_V2_STOPPED) : 0;
    return report(instance ? DFY_V2_OK : DFY_V2_INVALID_ARGUMENT,error);
}

// 已停止且释放全部工作后归还实例槽。
static void DFY_V2_CALL destroy(dfy_v2_instance* raw) {
    dfy_v2_instance* instance = owned(raw);
    if (instance && !instance->running && !instance->pending) instance->owner = 0;
}

// 返回固定工具数量，无效上下文清空输出。
static dfy_v2_status DFY_V2_CALL count(dfy_v2_instance* raw, uint32_t* output, dfy_v2_error* error) {
    const dfy_v2_instance* instance = owned(raw);
    if (!output) return report(DFY_V2_INVALID_ARGUMENT,error);
    *output = instance && instance->running ? (uint32_t)(sizeof(ids)/sizeof(ids[0])) : 0;
    return report(*output ? DFY_V2_OK : DFY_V2_INVALID_CONTEXT,error);
}

// 返回固定工具描述和变更标志。
// 步骤：核对输出布局 → 校验实例及索引 → 返回静态描述。
static dfy_v2_status DFY_V2_CALL describe(dfy_v2_instance* raw, uint32_t index, dfy_v2_tool_description* output, dfy_v2_error* error) {
    const dfy_v2_instance* instance = owned(raw);
    if (!output) return report(DFY_V2_INVALID_ARGUMENT,error);
    if (output->struct_size != sizeof(*output)) { output->struct_size = 0; return report(DFY_V2_ABI_MISMATCH,error); }
    memset(output,0,sizeof(*output));
    if (!instance || !instance->running) return report(DFY_V2_INVALID_CONTEXT,error);
    if (index >= sizeof(ids)/sizeof(ids[0])) return report(DFY_V2_NOT_FOUND,error);
    output->struct_size = sizeof(*output);
    output->version = DFY_V2_TOOL_VERSION;
    output->flags = index == read_mode ? 0 : DFY_V2_TOOL_CHANGES_STATE | DFY_V2_TOOL_IDEMPOTENT;
    output->id = view(ids[index]);
    output->description = view(descriptions[index]);
    output->parameters = view("{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}");
    return report(DFY_V2_OK,error);
}

// 接纳一次可取消的面板工具，不在接纳时变更状态。
// 步骤：核对调用和票据布局 → 解析固定动作 → 预留工作槽并签发原票据。
static dfy_v2_status DFY_V2_CALL begin(dfy_v2_instance* raw, dfy_v2_string_view id, const dfy_v2_tool_call* call,
    dfy_v2_tool_ticket* ticket, dfy_v2_error* error) {
    dfy_v2_instance* instance = owned(raw);
    if (!ticket) return report(DFY_V2_INVALID_ARGUMENT,error);
    memset(ticket,0,sizeof(*ticket));
    if (!instance || !instance->running || !call || call->struct_size != sizeof(*call)) return report(DFY_V2_INVALID_ARGUMENT,error);
    if (call->tool_version != DFY_V2_TOOL_VERSION) return report(DFY_V2_ABI_MISMATCH,error);
    if (!call->session || !call->run || !call->step || !call->attempt || !call->invocation ||
        !call->call_id.data || !call->call_id.length || call->call_id.length > 128 || !empty_object(call->arguments))
        return report(DFY_V2_INVALID_ARGUMENT,error);
    if (instance->pending) return report(DFY_V2_BUSY,error);
    if (instance->sequence == UINT64_MAX) return report(DFY_V2_RESOURCE_EXHAUSTED,error);
    size_t index = 0;
    while (index < sizeof(ids)/sizeof(ids[0]) && !equal(id,ids[index])) ++index;
    if (index == sizeof(ids)/sizeof(ids[0])) return report(DFY_V2_NOT_FOUND,error);

    instance->action = (enum action)index;
    instance->pending = 1;
    instance->ready = instance->exited = instance->cancelled = 0;
    instance->effect = DFY_V2_TOOL_NOT_APPLIED;
    ticket->owner = instance->owner;
    ticket->sequence = ++instance->sequence;
    return report(DFY_V2_OK,error);
}

// 推进一次面板操作，返回稳定结果及实际退出状态。
// 步骤：核对输出及原票据 → 首次结算动作或取消 → 后续确认退出并返回原结果。
static dfy_v2_status DFY_V2_CALL poll(dfy_v2_instance* raw, dfy_v2_tool_ticket ticket, dfy_v2_tool_progress* output, dfy_v2_error* error) {
    if (!output) return report(DFY_V2_INVALID_ARGUMENT,error);
    if (output->struct_size != sizeof(*output)) { output->struct_size = 0; return report(DFY_V2_ABI_MISMATCH,error); }
    memset(output,0,sizeof(*output));
    dfy_v2_instance* instance = work(raw,ticket);
    if (!instance) return report(DFY_V2_INVALID_HANDLE,error);
    if (instance->ready) instance->exited = 1;
    else {
        instance->ready = 1;
        const int target = instance->action == set_focus;
        if (!instance->cancelled && instance->action != read_mode && instance->focus != target) {
            instance->focus = target;
            instance->effect = DFY_V2_TOOL_APPLIED;
        }
    }

    output->struct_size = sizeof(*output);
    output->result_ready = 1;
    output->succeeded = !instance->cancelled;
    output->effect = instance->effect;
    output->exited = instance->exited;
    output->output = view(instance->cancelled ? "" : instance->focus ? "focus" : "idle");
    return report(DFY_V2_OK,error);
}

// 登记在结算前收到的协作取消。
static dfy_v2_status DFY_V2_CALL cancel(dfy_v2_instance* raw, dfy_v2_tool_ticket ticket, uint32_t reason, dfy_v2_error* error) {
    dfy_v2_instance* instance = work(raw,ticket);
    if (!instance) return report(DFY_V2_INVALID_HANDLE,error);
    if (reason < DFY_V2_CANCEL_USER || reason > DFY_V2_CANCEL_DEADLINE) return report(DFY_V2_INVALID_ARGUMENT,error);
    if (!instance->ready) instance->cancelled = 1;
    return report(DFY_V2_OK,error);
}

// 归还实际退出的工作票据。
static dfy_v2_status DFY_V2_CALL release(dfy_v2_instance* raw, dfy_v2_tool_ticket ticket, dfy_v2_error* error) {
    dfy_v2_instance* instance = work(raw,ticket);
    if (!instance) return report(DFY_V2_INVALID_HANDLE,error);
    if (!instance->exited) return report(DFY_V2_BUSY,error);
    instance->pending = 0;
    return report(DFY_V2_OK,error);
}

static const dfy_v2_lifecycle_api lifecycle = {{sizeof(dfy_v2_lifecycle_api),1},create,start,stop,stopped,destroy};
static const dfy_v2_tool_api tools = {{sizeof(dfy_v2_tool_api),DFY_V2_TOOL_VERSION},count,describe,begin,poll,cancel,release};

// 返回精确版本的工具或生命周期能力表。
static dfy_v2_status DFY_V2_CALL query(dfy_v2_string_view name, uint32_t version, const void** output, dfy_v2_error* error) {
    if (!output) return report(DFY_V2_INVALID_ARGUMENT,error);
    *output = NULL;
    if (version == 1 && equal(name,DFY_V2_LIFECYCLE_INTERFACE)) *output = &lifecycle;
    else if (version == DFY_V2_TOOL_VERSION && equal(name,DFY_V2_TOOL_INTERFACE)) *output = &tools;
    return report(*output ? DFY_V2_OK : DFY_V2_UNSUPPORTED,error);
}

static const dfy_v2_plugin_api plugin = {sizeof(dfy_v2_plugin_api),DFY_V2_ABI_VERSION,DFY_V2_ARCH_X64,0,query};
// 发布独立 C ABI v2 引导表。
DFY_V2_EXPORT dfy_v2_status DFY_V2_CALL dfy_plugin_query_v2(uint32_t version, const dfy_v2_plugin_api** output, dfy_v2_error* error) {
    if (!output) return report(DFY_V2_INVALID_ARGUMENT,error);
    *output = NULL;
    if (version != DFY_V2_ABI_VERSION) return report(DFY_V2_ABI_MISMATCH,error);
    *output = &plugin;
    return report(DFY_V2_OK,error);
}
