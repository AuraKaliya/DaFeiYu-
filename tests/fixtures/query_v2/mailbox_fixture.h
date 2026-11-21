#pragma once
#include <dafeiyu/lifecycle_abi_v2.h>
#include <dafeiyu/mailbox_abi_v2.h>
#include <dafeiyu/agent_abi_v2.h>
#define DFY_TEST_MAILBOX_INTERFACE "test.mailbox"
#pragma pack(push, 8)
typedef struct dfy_test_mailbox_api {
    dfy_v2_api_header header;
    // 消费一个请求并送回答复，只用于跨 DLL 测试。
    dfy_v2_status (DFY_V2_CALL *reply)(dfy_v2_instance*,dfy_v2_error*);
    // 提交固定子会话输入。
    dfy_v2_status (DFY_V2_CALL *ask)(dfy_v2_instance*,uint64_t*,dfy_v2_error*);
    // 读回固定子会话状态。
    dfy_v2_status (DFY_V2_CALL *agent_result)(dfy_v2_instance*,dfy_v2_agent_status*,dfy_v2_error*);
    // 按测试序号发出普通信件、待阻断事件或固定提问。
    dfy_v2_status (DFY_V2_CALL *emit_notice)(dfy_v2_instance*,uint64_t,int);
    // 读取处于有效范围内的阶段计数。
    uint32_t (DFY_V2_CALL *transition_count)(dfy_v2_instance*,uint32_t);
} dfy_test_mailbox_api;
#pragma pack(pop)
