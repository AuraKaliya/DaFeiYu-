#include "mailbox_fixture.h"
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <dafeiyu/hook_abi_v2.h>
#include <dafeiyu/activity_abi_v2.h>
#include <dafeiyu/routed_event_abi_v2.h>

_Static_assert(sizeof(dfy_v2_mailbox_api) == 32 && offsetof(dfy_v2_mailbox_api,poll) == 24,"mailbox ABI");
_Static_assert(sizeof(dfy_v2_mailbox_message) == 40 && offsetof(dfy_v2_mailbox_message,payload) == 16,"mailbox message ABI");
_Static_assert(sizeof(dfy_v2_hook_api) == 24 && sizeof(dfy_v2_hook_input) == 72,"hook ABI");
_Static_assert(sizeof(dfy_v2_text_hook_api) == 24 && sizeof(dfy_v2_text_hook_input) == 88,"text hook ABI");
_Static_assert(sizeof(dfy_v2_transition_hook_api) == 24 && sizeof(dfy_v2_transition_hook_input) == 96,"transition hook ABI");
_Static_assert(sizeof(dfy_v2_agent_api) == 48 && sizeof(dfy_v2_agent_input) == 64 && sizeof(dfy_v2_agent_status) == 32,"agent ABI");
_Static_assert(sizeof(dfy_v2_agent_facts_api) == 24 && sizeof(dfy_v2_fact_frame) == 40 && sizeof(dfy_v2_work_fact) == 56,"optional agent facts ABI");
_Static_assert(sizeof(dfy_v2_agent_ordered_facts_api) == 40,"ordered agent facts ABI");
_Static_assert(sizeof(dfy_v2_agent_memory_api)==32 && offsetof(dfy_v2_agent_memory_api,select_topic)==24,"agent memory ABI");
_Static_assert(sizeof(dfy_v2_agent_letters_api)==24,"agent letters ABI");
struct dfy_v2_instance {
    dfy_v2_query_interface_fn host;
    const dfy_v2_mailbox_api* mail;
    const dfy_v2_mailbox_text_api* text;
    const dfy_v2_reference_api* refs;
    const dfy_v2_agent_api* agent;
    const dfy_v2_routed_event_api* events;
    uint64_t session;
    uint32_t observed[DFY_V2_TRANSITION_RUN_EXITED];
    char converter;
};

// 把两种测试事件交给普通信箱路由。
static dfy_v2_status DFY_V2_CALL notice(void* context, dfy_v2_string_view payload, dfy_v2_event_projection* out) {
    (void)context;
    out->kind = DFY_V2_EVENT_MAIL;
    out->length = (uint32_t)payload.length;
    memcpy(out->json,payload.data,out->length);
    return DFY_V2_OK;
}
// 产生固定终端提问，供宿主直接路径测试。
static dfy_v2_status DFY_V2_CALL console_question(void* context, dfy_v2_string_view payload, dfy_v2_event_projection* out) {
    const char* body = "{\"item\":1,\"revision\":1,\"waitMs\":1000,\"question\":\"proceed?\",\"recipient\":\"owner\",\"channel\":\"terminal\"}";
    (void)context;
    (void)payload;
    out->kind = DFY_V2_EVENT_ASK;
    out->length = (uint32_t)strlen(body);
    memcpy(out->json,body,out->length);
    return DFY_V2_OK;
}

// 为纯 C 插件分配独立实例并保存宿主查询入口。
static dfy_v2_status DFY_V2_CALL create(dfy_v2_query_interface_fn host, dfy_v2_string_view config,
    dfy_v2_instance** out, dfy_v2_error* error) {
    if (error) error->length = 0;
    *out = calloc(1,sizeof(**out));
    if (!*out) return DFY_V2_RESOURCE_EXHAUSTED;
    (*out)->host = host;
    if (config.length == 12 && !memcmp(config.data,"{\"mode\":\"",9)) (*out)->converter = config.data[9];
    return DFY_V2_OK;
}

// 核对旧服务拒绝新标志，新服务拒绝同一请求同时关闭和开启思考。
// 步骤：协商精确版本 → 提交该版本不接受的标志 → 确认宿主未接纳请求。
static dfy_v2_status check_thinking_flags(dfy_v2_instance* instance) {
    for (uint32_t version = DFY_V2_AGENT_VERSION; version <= DFY_V2_AGENT_THINKING_VERSION; ++version) {
        const void* table = NULL;
        const dfy_v2_status queried = instance->host((dfy_v2_string_view){DFY_V2_AGENT_INTERFACE,sizeof(DFY_V2_AGENT_INTERFACE)-1},
            version,&table,NULL);
        if (queried == DFY_V2_UNSUPPORTED) continue;
        if (queried || !table) return DFY_V2_ABI_MISMATCH;

        const dfy_v2_agent_api* service = table;
        dfy_v2_agent_input input = {{"flags",5},{0},{0},NULL,0,DFY_V2_AGENT_THINKING_LOW};
        if (version == DFY_V2_AGENT_THINKING_VERSION) input.flags |= DFY_V2_AGENT_THINKING_OFF;
        uint64_t request = 0;
        if (service->header.interface_version != version ||
            service->submit(instance->session,&input,&request,NULL) != DFY_V2_INVALID_ARGUMENT || request)
            return DFY_V2_ABI_MISMATCH;
    }
    return DFY_V2_OK;
}

// 在实例载入阶段显式注册双向信箱。
// 步骤：协商信箱及引用版本 → 校验布局 → 接入并保留接口。
static dfy_v2_status DFY_V2_CALL start(dfy_v2_instance* instance, dfy_v2_error* error) {
    const void *mail = NULL, *refs = NULL, *agent = NULL, *text = NULL;
    dfy_v2_mailbox_pair addresses = {0};
    dfy_v2_status status = DFY_V2_OK;
    if (instance->host((dfy_v2_string_view){DFY_V2_MAILBOX_INTERFACE,sizeof(DFY_V2_MAILBOX_INTERFACE)-1},1,&mail,error) ||
        instance->host((dfy_v2_string_view){DFY_V2_MAILBOX_TEXT_INTERFACE,sizeof(DFY_V2_MAILBOX_TEXT_INTERFACE)-1},1,&text,error) ||
        instance->host((dfy_v2_string_view){DFY_V2_REFERENCE_INTERFACE,sizeof(DFY_V2_REFERENCE_INTERFACE)-1},1,&refs,error))
        return DFY_V2_UNSUPPORTED;
    if (!mail || !refs || ((const dfy_v2_api_header*)mail)->struct_size != sizeof(dfy_v2_mailbox_api) ||
        ((const dfy_v2_api_header*)refs)->struct_size != sizeof(dfy_v2_reference_api)) return DFY_V2_UNSUPPORTED;
    instance->mail = mail;
    instance->text = text;
    instance->refs = refs;
    status = instance->mail->attach(&addresses,error);
    if (!status && instance->converter == 't') {
        const void *events = NULL, *routes = NULL;
        status = instance->host((dfy_v2_string_view){DFY_V2_ROUTED_EVENT_INTERFACE,sizeof(DFY_V2_ROUTED_EVENT_INTERFACE)-1},1,&events,error);
        if (!status) status = instance->host((dfy_v2_string_view){DFY_V2_ROUTED_MAILBOX_INTERFACE,sizeof(DFY_V2_ROUTED_MAILBOX_INTERFACE)-1},1,&routes,error);
        if (status) return status;
        instance->events = events;
        status = ((const dfy_v2_routed_mailbox_api*)routes)->register_mailbox((dfy_v2_string_view){"test.notice",11},1,DFY_V2_MAIL_WAKE,error);
        if (!status) status = instance->events->register_event((dfy_v2_string_view){"test.notice",11},1,(dfy_v2_string_view){"{}",2},notice,NULL,error);
        if (!status) status = instance->events->register_event((dfy_v2_string_view){"test.deny",9},1,(dfy_v2_string_view){"{}",2},notice,NULL,error);
        if (!status) status = instance->events->register_event((dfy_v2_string_view){"test.ask",8},1,(dfy_v2_string_view){"{}",2},console_question,NULL,error);
        if (status) return status;
    }
    if (status) return status;
    status = instance->host((dfy_v2_string_view){DFY_V2_AGENT_INTERFACE,sizeof(DFY_V2_AGENT_INTERFACE)-1},1,&agent,error);
    if (status == DFY_V2_UNSUPPORTED) return DFY_V2_OK;
    if (status || !agent || ((const dfy_v2_api_header*)agent)->struct_size != sizeof(dfy_v2_agent_api)) return DFY_V2_UNSUPPORTED;
    instance->agent = agent;
    status = instance->agent->open(&instance->session,error);
    return status ? status : check_thinking_flags(instance);
}

// 消费真实队首并按原测试序号向本体回信。
// 步骤：取件并读取原分类和正文 → 释放独立引用 → 发送对应回信。
static dfy_v2_status DFY_V2_CALL reply(dfy_v2_instance* instance, dfy_v2_error* error) {
    dfy_v2_mailbox_message input = {0};
    dfy_v2_mailbox_text body = {0};
    dfy_v2_mailbox_receipt sent = {0};
    dfy_v2_status status = instance->mail->poll(&input,error);
    if (status) return status;
    status = instance->text->inspect(input.payload,&body,error);
    if (status || body.version != 1 || body.category.length != 12 || memcmp(body.category.data,"test.request",12) ||
        body.text.length != 6 || memcmp(body.text.data,"source",6)) {
        instance->refs->release(input.payload,error);
        return DFY_V2_FAILED;
    }
    status = instance->refs->release(input.payload,error);
    if (status) return status;
    return instance->mail->post(input.sequence,(dfy_v2_string_view){"test.reply",10},1,(dfy_v2_string_view){"received",8},&sent,error);
}

// 以纯 C 接口向启动时建立的原会话提交一轮。
static dfy_v2_status DFY_V2_CALL ask(dfy_v2_instance* instance, uint64_t* request, dfy_v2_error* error) {
    const dfy_v2_agent_input input = {{"fixture",7},{0},{0},NULL,0,0};
    return instance->agent->submit(instance->session,&input,request,error);
}

// 返回原子会话的状态与借用正文。
static dfy_v2_status DFY_V2_CALL agent_result(dfy_v2_instance* instance, dfy_v2_agent_status* result, dfy_v2_error* error) {
    return instance->agent->status(instance->session,result,error);
}

// 夹具没有背景动作，停止提交可直接接受。
static dfy_v2_status DFY_V2_CALL stop(dfy_v2_instance* instance, dfy_v2_error* error) {
    (void)instance;
    if (error) error->length = 0;
    return DFY_V2_OK;
}
// 报告本夹具已停止，宿主另行撤销信箱。
static dfy_v2_status DFY_V2_CALL stopped(dfy_v2_instance* instance, uint32_t* out, dfy_v2_error* error) {
    *out = DFY_V2_STOPPED;
    return stop(instance,error);
}
// 由分配该实例的 DLL 归还私有内存。
static void DFY_V2_CALL destroy(dfy_v2_instance* instance) { free(instance); }
static const dfy_v2_lifecycle_api lifecycle = {{sizeof(dfy_v2_lifecycle_api),1},create,start,stop,stopped,destroy};
// 通过正常事件入口发出一个测试通知。
static dfy_v2_status DFY_V2_CALL emit_notice(dfy_v2_instance* instance, uint64_t sequence, int block) {
    const dfy_v2_string_view type = block == 2 ? (dfy_v2_string_view){"test.ask",8} :
        block ? (dfy_v2_string_view){"test.deny",9} : (dfy_v2_string_view){"test.notice",11};
    dfy_v2_event_receipt receipt = {0};
    return instance->events->emit(type,1,sequence,(dfy_v2_string_view){"{}",2},&receipt,NULL);
}
// 读取原实例实际观察到的系统阶段次数。
static uint32_t DFY_V2_CALL transition_count(dfy_v2_instance* instance, uint32_t point) {
    return instance->observed[point-1];
}
// 以同一原生实例读回实际回执，缓冲不足和旧代次查询都不改变工作。
// 步骤：协商第二版 → 提交原件及最终实效 → 核对原号重传 → 检查输出边界及实收正文。
static dfy_v2_status DFY_V2_CALL work_receipts(dfy_v2_instance* instance, uint64_t session, uint64_t epoch) {
    const void* table = NULL;
    const dfy_v2_string_view name = {DFY_V2_AGENT_FACTS_INTERFACE,sizeof(DFY_V2_AGENT_FACTS_INTERFACE)-1};
    if (instance->host(name,DFY_V2_AGENT_ORDERED_FACTS_VERSION,&table,NULL) || !table) return DFY_V2_UNSUPPORTED;
    const dfy_v2_agent_ordered_facts_api* service = table;
    if (service->base.header.struct_size != sizeof(*service) ||
        service->base.header.interface_version != DFY_V2_AGENT_ORDERED_FACTS_VERSION) return DFY_V2_ABI_MISMATCH;

    dfy_v2_work_fact fact = {epoch,1,{"c-fixture",9},{"{}",2},DFY_V2_WORK_ACCEPTED,0};
    uint64_t accepted = 0, effect = 0, retry = 0;
    if (service->settle_ordered(session,&fact,0,0,&accepted,NULL)!=DFY_V2_INVALID_ARGUMENT || accepted) return DFY_V2_FAILED;
    if (service->settle_ordered(session,&fact,1,0,&accepted,NULL) || !accepted) return DFY_V2_FAILED;
    fact.revision = 2; fact.phase = DFY_V2_WORK_EFFECT;
    fact.content = (dfy_v2_string_view){"{\"picked\":8}",12};
    if (service->settle_ordered(session,&fact,0,DFY_V2_WORK_FINAL_EFFECT,&effect,NULL) || effect <= accepted ||
        service->settle_ordered(session,&fact,0,DFY_V2_WORK_FINAL_EFFECT,&retry,NULL) || retry != effect) return DFY_V2_FAILED;

    uint32_t needed = 0, written = 0;
    char output[512] = {'?'};
    if (service->read_receipt(session,epoch,effect,output,1,&needed,NULL) != DFY_V2_RESOURCE_EXHAUSTED ||
        output[0] != '?' || !needed || needed >= sizeof(output) ||
        service->read_receipt(session,epoch+1,effect,output,sizeof(output),&written,NULL) != DFY_V2_NOT_FOUND || written ||
        service->read_receipt(session,epoch,effect,output,needed,&written,NULL) || written != needed) return DFY_V2_FAILED;
    output[written] = 0;
    return strstr(output,"\"body\":{\"picked\":8}") && strstr(output,"\"finalEffect\":true") ? DFY_V2_OK : DFY_V2_FAILED;
}

static const dfy_test_mailbox_api exercise = {{sizeof(dfy_test_mailbox_api),1},reply,ask,agent_result,emit_notice,transition_count,work_receipts};

// 依次注册主会话模型前检查及工具确认。
static dfy_v2_status DFY_V2_CALL describe_hook(dfy_v2_instance* instance, uint32_t index,
    dfy_v2_hook_description* out, dfy_v2_error* error) {
    if (error) error->length = 0;
    *out = (dfy_v2_hook_description){0};
    if (instance->converter == 't') return DFY_V2_NOT_FOUND;
    if (index == 0) *out = (dfy_v2_hook_description){DFY_V2_HOOK_MODEL_REQUEST,DFY_V2_HOOK_MAIN_SESSION};
    if (index == 1) *out = (dfy_v2_hook_description){DFY_V2_HOOK_TOOL_START,DFY_V2_HOOK_MAIN_SESSION|DFY_V2_HOOK_OWN_SESSION};
    return out->point ? DFY_V2_OK : DFY_V2_NOT_FOUND;
}

// 按原注册返回决定，并验证宿主调用传入原实例和有效会话。
static dfy_v2_status DFY_V2_CALL decide_hook(dfy_v2_instance* instance, uint32_t index,
    const dfy_v2_hook_input* input, uint32_t* verdict, dfy_v2_error* error) {
    if (error) error->length = 0;
    *verdict = 0;
    if (!input->session || !instance->mail) return DFY_V2_INVALID_ARGUMENT;
    *verdict = index == 1 ? DFY_V2_HOOK_CONFIRM :
        (!input->owner && input->run == 2 ? DFY_V2_HOOK_DENY : DFY_V2_HOOK_ALLOW);
    return DFY_V2_OK;
}
static const dfy_v2_hook_api hooks = {{sizeof(dfy_v2_hook_api),1},describe_hook,decide_hook};

// 独立或递进转换均在启动阶段注册，运行时只检查字段名。
static dfy_v2_status DFY_V2_CALL describe_text(dfy_v2_instance* instance, uint32_t index,
    dfy_v2_text_hook_description* out, dfy_v2_error* error) {
    if (error) error->length = 0;
    *out = (dfy_v2_text_hook_description){0};
    if (instance->converter == 'e' && index < DFY_V2_TEXT_HOOK_REPLY) {
        *out = (dfy_v2_text_hook_description){index+1,DFY_V2_HOOK_MAIN_SESSION,0};
        return DFY_V2_OK;
    }
    if (index || !instance->converter) return DFY_V2_NOT_FOUND;
    *out = (dfy_v2_text_hook_description){DFY_V2_TEXT_HOOK_INPUT,DFY_V2_HOOK_MAIN_SESSION,
        instance->converter == 'b' ? DFY_V2_TEXT_HOOK_CHAIN : 0};
    return DFY_V2_OK;
}

// 在正式回合中分别验证入轮、请求、响应、工具候选和回复投递字段。
// 步骤：根据阶段匹配原值 → 决定目标正文 → 把结果复制到宿主缓冲。
static dfy_v2_status rewrite_turn(uint32_t index, dfy_v2_string_view value, char* output, uint64_t capacity, uint64_t* written) {
    const char* target = NULL;
    if (index == 0 && value.length == 10 && !memcmp(value.data,"hook input",10)) target = "rewritten prompt";
    // 该夹具明确接管系统段，同时覆盖空系统段和宿主渲染后的系统段。
    if (index == 1) target = "hook system";
    if (index == 2 && value.length == 3 && !memcmp(value.data,"raw",3)) target = "model text";
    if (index == 3 && value.length == 9 && !memcmp(value.data,"{\"bad\":1}",9)) target = "{}";
    if (index == 4 && value.length == 10 && !memcmp(value.data,"model text",10)) target = "visible reply";
    if (target) {
        const size_t size = strlen(target);
        if (size > capacity) return DFY_V2_RESOURCE_EXHAUSTED;
        memcpy(output,target,size);
        *written = size;
    }
    return DFY_V2_OK;
}

// 给定不同实例配置，验证同源、串行接续、平行冲突及非法回调结果。
// 步骤：匹配字段 → 校验输入来源 → 写出声明的结果。
static dfy_v2_status DFY_V2_CALL rewrite_text(dfy_v2_instance* instance, uint32_t index,
    const dfy_v2_text_hook_input* input, char* output, uint64_t capacity, uint64_t* written, dfy_v2_error* error) {
    if (error) error->length = 0;
    *written = DFY_V2_TEXT_HOOK_UNCHANGED;
    if (!input->session || !output || !capacity) return DFY_V2_INVALID_ARGUMENT;
    if (instance->converter == 'e') return rewrite_turn(index,input->value,output,capacity,written);
    if (index) return DFY_V2_INVALID_ARGUMENT;
    if (input->field.length != 5 || memcmp(input->field.data,"probe",5)) return DFY_V2_OK;
    if (instance->converter == 'z') { *written = capacity+1; return DFY_V2_OK; }
    if (input->value.length != 1) return DFY_V2_FAILED;
    if (instance->converter == 'b' ? input->value.data[0] != 'a' : input->value.data[0] != '?') return DFY_V2_FAILED;
    output[0] = instance->converter == 'd' ? 'a' : instance->converter;
    *written = 1;
    return DFY_V2_OK;
}
static const dfy_v2_text_hook_api text_hooks = {{sizeof(dfy_v2_text_hook_api),1},describe_text,rewrite_text};

// 在测试模式登记所有系统转换，供本体与原实例事件验证。
static dfy_v2_status DFY_V2_CALL describe_transition(dfy_v2_instance* instance, uint32_t index,
    dfy_v2_hook_description* out, dfy_v2_error* error) {
    if (error) error->length = 0;
    *out = (dfy_v2_hook_description){0};
    if (instance->converter != 't' || index >= DFY_V2_TRANSITION_RUN_EXITED) return DFY_V2_NOT_FOUND;
    *out = (dfy_v2_hook_description){index+1,DFY_V2_HOOK_MAIN_SESSION|DFY_V2_HOOK_OWN_SESSION|DFY_V2_HOOK_OTHER_SESSION};
    return DFY_V2_OK;
}

// 验证系统阶段对三种提案的权限。
// 步骤：记录阶段 → 尝试拒绝测试事件或只读通知 → 在普通邮件上改写模型副本。
static dfy_v2_status DFY_V2_CALL apply_transition(dfy_v2_instance* instance, uint32_t index,
    const dfy_v2_transition_hook_input* input, char* output, uint64_t capacity, uint32_t* verdict, uint64_t* written,
    dfy_v2_error* error) {
    if (error) error->length = 0;
    ++instance->observed[index];
    if (input->point == DFY_V2_TRANSITION_EVENT_BEFORE_DISPATCH && input->category.length == 9 &&
        !memcmp(input->category.data,"test.deny",9)) *verdict = DFY_V2_HOOK_DENY;
    if (!input->permissions) *verdict = DFY_V2_HOOK_DENY;
    if (input->point == DFY_V2_TRANSITION_MAIL_CONTEXT && input->permissions) {
        if (capacity < 8) return DFY_V2_INVALID_ARGUMENT;
        memcpy(output,"adjusted",8);
        *written = 8;
    }
    return DFY_V2_OK;
}
static const dfy_v2_transition_hook_api transitions = {{sizeof(dfy_v2_transition_hook_api),1},describe_transition,apply_transition};

// 每步最多消费一封本体来信；信箱为空时等下一次推进。
static dfy_v2_status DFY_V2_CALL step(dfy_v2_instance* instance, uint32_t* wait_ms, dfy_v2_error* error) {
    *wait_ms = 1;
    if (instance->converter == 't') return DFY_V2_OK;
    const dfy_v2_status status = reply(instance,error);
    return status == DFY_V2_NOT_FOUND ? DFY_V2_OK : status;
}
static const dfy_v2_activity_api activity = {{sizeof(dfy_v2_activity_api),1},step};

// 按精确名称和版本返回生命周期、Hook 或测试驱动接口。
static dfy_v2_status DFY_V2_CALL query(dfy_v2_string_view name, uint32_t version, const void** out, dfy_v2_error* error) {
    *out = NULL;
    if (error) error->length = 0;
    if (!name.data || version != 1) return DFY_V2_UNSUPPORTED;
    if (name.length == sizeof(DFY_V2_LIFECYCLE_INTERFACE)-1 && !memcmp(name.data,DFY_V2_LIFECYCLE_INTERFACE,name.length))
        *out = &lifecycle;
    if (name.length == sizeof(DFY_TEST_MAILBOX_INTERFACE)-1 && !memcmp(name.data,DFY_TEST_MAILBOX_INTERFACE,name.length))
        *out = &exercise;
    if (name.length == sizeof(DFY_V2_HOOK_INTERFACE)-1 && !memcmp(name.data,DFY_V2_HOOK_INTERFACE,name.length))
        *out = &hooks;
    if (name.length == sizeof(DFY_V2_TEXT_HOOK_INTERFACE)-1 && !memcmp(name.data,DFY_V2_TEXT_HOOK_INTERFACE,name.length))
        *out = &text_hooks;
    if (name.length == sizeof(DFY_V2_TRANSITION_HOOK_INTERFACE)-1 && !memcmp(name.data,DFY_V2_TRANSITION_HOOK_INTERFACE,name.length))
        *out = &transitions;
    if (name.length == sizeof(DFY_V2_ACTIVITY_INTERFACE)-1 && !memcmp(name.data,DFY_V2_ACTIVITY_INTERFACE,name.length))
        *out = &activity;
    return *out ? DFY_V2_OK : DFY_V2_UNSUPPORTED;
}

// 发布夹具 v2 引导表。
DFY_V2_EXPORT dfy_v2_status DFY_V2_CALL dfy_plugin_query_v2(uint32_t abi, const dfy_v2_plugin_api** out, dfy_v2_error* error) {
    static const dfy_v2_plugin_api table = {sizeof(dfy_v2_plugin_api),DFY_V2_ABI_VERSION,DFY_V2_ARCH_X64,0,query};
    if (error) error->length = 0;
    *out = abi == DFY_V2_ABI_VERSION ? &table : NULL;
    return *out ? DFY_V2_OK : DFY_V2_ABI_MISMATCH;
}
