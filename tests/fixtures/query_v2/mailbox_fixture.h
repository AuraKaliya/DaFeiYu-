#pragma once
#include <dafeiyu/lifecycle_abi_v2.h>
#include <dafeiyu/mailbox_abi_v2.h>
#define DFY_TEST_MAILBOX_INTERFACE "test.mailbox"
#pragma pack(push, 8)
typedef struct dfy_test_mailbox_api {
    dfy_v2_api_header header;
    // 消费一个请求并送回答复，只用于跨 DLL 测试。
    dfy_v2_status (DFY_V2_CALL *reply)(dfy_v2_instance*,dfy_v2_error*);
} dfy_test_mailbox_api;
#pragma pack(pop)
