#pragma once
#include <dafeiyu/plugin_abi_v2.h>

#define DFY_V2_AGENT_INTERFACE "dfy.agent"
#define DFY_V2_AGENT_VERSION UINT32_C(1)
#define DFY_V2_AGENT_PERSONA_VERSION UINT32_C(2)
#define DFY_V2_AGENT_THINKING_VERSION UINT32_C(3)
#define DFY_V2_AGENT_MEMORY_INTERFACE "dfy.agent.memory"
#define DFY_V2_AGENT_MEMORY_VERSION UINT32_C(1)
#define DFY_V2_MEMORY_TASK UINT32_C(1)
#define DFY_V2_MEMORY_BUSINESS UINT32_C(2)
#define DFY_V2_MEMORY_AVATAR UINT32_C(3)
#define DFY_V2_AGENT_LETTERS_INTERFACE "dfy.agent.letters"
#define DFY_V2_AGENT_LETTERS_VERSION UINT32_C(1)
#define DFY_V2_LETTER_STATUS UINT32_C(1)
#define DFY_V2_LETTER_SUMMARY UINT32_C(2)
#define DFY_V2_LETTER_ORIGINAL UINT32_C(3)
#define DFY_V2_LETTER_PERSONA UINT32_C(1)
#define DFY_V2_AGENT_KNOWLEDGE_INTERFACE "dfy.agent.knowledge"
#define DFY_V2_AGENT_KNOWLEDGE_VERSION UINT32_C(1)
#define DFY_V2_AGENT_FACTS_INTERFACE "dfy.agent.facts"
#define DFY_V2_AGENT_FACTS_VERSION UINT32_C(1)
#define DFY_V2_AGENT_ORDERED_FACTS_VERSION UINT32_C(2)
#define DFY_V2_WORK_FINAL_EFFECT UINT32_C(1)
#define DFY_V2_FACT_BASELINE UINT32_C(1)
#define DFY_V2_WORK_ACCEPTED UINT32_C(1)
#define DFY_V2_WORK_EFFECT UINT32_C(2)
#define DFY_V2_WORK_EXIT UINT32_C(3)
#define DFY_V2_AGENT_IDLE UINT32_C(1)
#define DFY_V2_AGENT_QUEUED UINT32_C(2)
#define DFY_V2_AGENT_RUNNING UINT32_C(3)
#define DFY_V2_AGENT_SUCCEEDED UINT32_C(4)
#define DFY_V2_AGENT_FAILED UINT32_C(5)
#define DFY_V2_AGENT_CANCELLED UINT32_C(6)
#define DFY_V2_AGENT_ALLOW_CHANGES UINT32_C(1)
#define DFY_V2_AGENT_REPLACE_HISTORY UINT32_C(2)
// 只在第 2 版服务请求把本体人格用于面向玩家的回合。
#define DFY_V2_AGENT_PLAYER_FACING UINT32_C(4)
// 第 3 版可为本次请求选择关闭或低强度思考，两者互斥，省略时继承宿主配置。
#define DFY_V2_AGENT_THINKING_OFF UINT32_C(8)
#define DFY_V2_AGENT_THINKING_LOW UINT32_C(16)

#ifdef __cplusplus
extern "C" {
#endif
#pragma pack(push, 8)
typedef struct dfy_v2_agent_input {
    dfy_v2_string_view prompt;
    dfy_v2_string_view instructions;
    dfy_v2_string_view binding;
    const dfy_v2_string_view* tools;
    uint32_t tool_count;
    uint32_t flags;
} dfy_v2_agent_input;
typedef struct dfy_v2_agent_status {
    uint64_t request;
    uint32_t phase;
    uint32_t reserved;
    dfy_v2_string_view answer;
} dfy_v2_agent_status;
typedef struct dfy_v2_agent_api {
    dfy_v2_api_header header;
    // 在启动或活动回调中创建独立上下文，宿主绑定调用实例。
    dfy_v2_status (DFY_V2_CALL *open)(uint64_t* session, dfy_v2_error* error);
    // 复制输入并排队，工具必须属于原插件实例；返回前不分派模型或工具。
    dfy_v2_status (DFY_V2_CALL *submit)(uint64_t session, const dfy_v2_agent_input* input, uint64_t* request, dfy_v2_error* error);
    // 查询原会话最新请求；答案只借用至本次插件回调返回或同会话修改，以较早者为准。
    dfy_v2_status (DFY_V2_CALL *status)(uint64_t session, dfy_v2_agent_status* status, dfy_v2_error* error);
    // 取消原请求；过期或已结算请求返回 NOT_FOUND，取消后仍需等待实际退出。
    dfy_v2_status (DFY_V2_CALL *cancel)(uint64_t session, uint64_t request, dfy_v2_error* error);
    // 请求关闭整个上下文，宿主推进退出；同实例忙碌时卸载返回 BUSY。
    dfy_v2_status (DFY_V2_CALL *close)(uint64_t session, dfy_v2_error* error);
} dfy_v2_agent_api;
typedef struct dfy_v2_agent_memory_api {
    dfy_v2_api_header header;
    // 在原插件回调内打开持久身份。subject 为委托号、业务对象或分身账号；只有分身填写 channel。
    dfy_v2_status (DFY_V2_CALL *open_bound)(uint32_t preset, dfy_v2_string_view subject,
        dfy_v2_string_view channel, uint64_t* session, dfy_v2_error* error);
    // 同步存入原子会话自己的记录，同 event 同内容重试沿用原件号，冲突或越权失败。
    dfy_v2_status (DFY_V2_CALL *remember)(uint64_t session, dfy_v2_string_view event, dfy_v2_string_view topic,
        dfy_v2_string_view content, uint64_t* original, dfy_v2_error* error);
    // 为该会话选择送模的精确主题，空值清除选择；读取和向当前受众转述由宿主在实际提交时复核。
    dfy_v2_status (DFY_V2_CALL *select_topic)(uint64_t session, dfy_v2_string_view topic, dfy_v2_error* error);
} dfy_v2_agent_memory_api;
typedef struct dfy_v2_agent_letters_api {
    dfy_v2_api_header header;
    // 为原持久子会话寄启动信。exchange 是同一委托的稳定交付号，模式及人格选择在首次寄信时固定。
    dfy_v2_status (DFY_V2_CALL *begin)(uint64_t session,dfy_v2_string_view exchange,uint32_t mode,uint32_t flags,
        uint64_t* receipt,dfy_v2_error* error);
    // 在实际终态后寄结束信，可选一份自己的笔记显式导出。相同交付重试沿用原信号。
    dfy_v2_status (DFY_V2_CALL *finish)(uint64_t session,dfy_v2_string_view summary,dfy_v2_string_view body,
        uint64_t note,uint64_t* receipt,dfy_v2_error* error);
} dfy_v2_agent_letters_api;
typedef struct dfy_v2_agent_knowledge_api {
    dfy_v2_api_header header;
    // 为持久会话选择已发布知识的主题，替换此前的记忆选择；宿主按版本和受众核对权限。
    dfy_v2_status (DFY_V2_CALL *select_topic)(uint64_t session,dfy_v2_string_view topic,dfy_v2_error* error);
} dfy_v2_agent_knowledge_api;
// scope 标明插件侧玩家和世界，实际所有者及会话由宿主绑定。序号与代次均从非零值开始。
typedef struct dfy_v2_fact_frame {
    uint64_t epoch, sequence;
    dfy_v2_string_view scope;
    uint32_t flags, reserved;
} dfy_v2_fact_frame;
// 工作用原号和代次结算；阶段表示接纳、实效或退出，不从文本推断实收。
typedef struct dfy_v2_work_fact {
    uint64_t epoch, revision;
    dfy_v2_string_view work, content;
    uint32_t phase, reserved;
} dfy_v2_work_fact;
typedef struct dfy_v2_agent_facts_api {
    dfy_v2_api_header header;
    // 复制 input.prompt 内的 set／closed 字段补丁并排队。重复原件返回原请求号，断序返回 NOT_FOUND 和下一序号。
    dfy_v2_status (DFY_V2_CALL *submit)(uint64_t session, const dfy_v2_agent_input* input,
        const dfy_v2_fact_frame* frame, uint64_t* request, uint64_t* expected, dfy_v2_error* error);
    // 同步复制原工作回执并返回接纳号，不分派动作。旧代次只可结算已经登记的原工作。
    dfy_v2_status (DFY_V2_CALL *settle)(uint64_t session, const dfy_v2_work_fact* fact, uint64_t* receipt, dfy_v2_error* error);
} dfy_v2_agent_facts_api;
// 第二版沿用事实提交和旧工作接口，按需启用有序接纳及指定接纳号的只读查询。
typedef struct dfy_v2_agent_ordered_facts_api {
    dfy_v2_agent_facts_api base;
    // 首次接纳使用本代次递增序号，重传须保持原号。后续阶段序号为零，最终实效可标记 FINAL_EFFECT。
    dfy_v2_status (DFY_V2_CALL *settle_ordered)(uint64_t session, const dfy_v2_work_fact* fact,
        uint64_t admission_sequence, uint32_t flags, uint64_t* receipt, dfy_v2_error* error);
    // 只读原代次接纳号的 JSON，written 返回所需字节数。缓冲不足时不写入，归档项返回正文指纹。
    dfy_v2_status (DFY_V2_CALL *read_receipt)(uint64_t session, uint64_t epoch, uint64_t receipt,
        char* output, uint32_t capacity, uint32_t* written, dfy_v2_error* error);
} dfy_v2_agent_ordered_facts_api;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
