#pragma once
#include <dafeiyu/lifecycle_abi_v2.h>
#define DFY_V2_TOOL_INTERFACE "dfy.tools"
#define DFY_V2_TOOL_VERSION UINT32_C(1)
#define DFY_V2_TOOL_CHANGES_STATE UINT32_C(1)
#define DFY_V2_TOOL_IDEMPOTENT UINT32_C(2)
#define DFY_V2_TOOL_NOT_APPLIED UINT32_C(1)
#define DFY_V2_TOOL_APPLIED UINT32_C(2)
#define DFY_V2_TOOL_UNKNOWN UINT32_C(3)
#ifdef __cplusplus
extern "C" {
#endif
#pragma pack(push, 8)
// 提供方私有工作票据，不是宿主引用句柄；两字段非零，在实例内及槽复用后不能混用旧票据。
typedef struct dfy_v2_tool_ticket { uint64_t owner; uint64_t sequence; } dfy_v2_tool_ticket;
// 返回字段由宿主复制；字符串借用到下一次插件调用，期间必须保持实例与代码持有。
typedef struct dfy_v2_tool_description {
    uint32_t struct_size;
    uint32_t version;
    uint32_t flags;
    uint32_t reserved;
    // 实例内局部 ID：1～32 个 ASCII 字母、数字、下划线或连字符，由宿主加包命名空间。
    dfy_v2_string_view id;
    dfy_v2_string_view description;
    dfy_v2_string_view parameters;
} dfy_v2_tool_description;
// 输入只在 begin 调用期间借用，后台工作自行复制所需字段；运行及调用 ID 关联命令，invocation 区分原生尝试。
typedef struct dfy_v2_tool_call {
    uint32_t struct_size;
    uint32_t tool_version;
    uint64_t session;
    uint64_t run;
    uint64_t step;
    uint64_t attempt;
    uint64_t invocation;
    dfy_v2_string_view call_id;
    dfy_v2_string_view arguments;
} dfy_v2_tool_call;
// 布尔字段只接受 0／1；result_ready 后结果保持不变，exited 单调变为 1；输出借用到下一次插件调用。
typedef struct dfy_v2_tool_progress {
    uint32_t struct_size;
    uint32_t result_ready;
    uint32_t succeeded;
    uint32_t effect;
    uint32_t exited;
    uint32_t reserved;
    dfy_v2_string_view output;
} dfy_v2_tool_progress;
// 可选原生工具能力；所有入口在所属实例的宿主线程、服务绑定和代码活动保护内调用，不得抛出异常。
typedef struct dfy_v2_tool_api {
    dfy_v2_api_header header;
    // 只读返回本实例固定描述数量，最多 64；输出必需，失败清零，不执行工具。
    dfy_v2_status (DFY_V2_CALL *count)(dfy_v2_instance* instance, uint32_t* count, dfy_v2_error* error);
    // 按零基索引返回描述；先核对 struct_size，大小不符仅清零该字段，其他失败清零完整输出；不修改能力集合。
    dfy_v2_status (DFY_V2_CALL *describe)(dfy_v2_instance* instance, uint32_t index, dfy_v2_tool_description* description, dfy_v2_error* error);
    // 非阻塞校验版本并接纳工作，成功返回非零票据；失败无副作用且无后台责任，票据清零。
    dfy_v2_status (DFY_V2_CALL *begin)(dfy_v2_instance* instance, dfy_v2_string_view id,
        const dfy_v2_tool_call* call, dfy_v2_tool_ticket* ticket, dfy_v2_error* error);
    // 非阻塞推进并复制状态；先核对 struct_size，大小不符仅清零该字段，其他失败清零输出且保留工作责任。
    dfy_v2_status (DFY_V2_CALL *poll)(dfy_v2_instance* instance, dfy_v2_tool_ticket ticket,
        dfy_v2_tool_progress* progress, dfy_v2_error* error);
    // 幂等提交协作取消，原因使用 DFY_V2_CANCEL_*；OK 不表示业务已取消或工作已退出。
    dfy_v2_status (DFY_V2_CALL *request_cancel)(dfy_v2_instance* instance, dfy_v2_tool_ticket ticket,
        uint32_t reason, dfy_v2_error* error);
    // 仅在 exited 后释放票据；失败不释放且允许重试，成功后旧票据失效；实例和代码必须持有到此成功。
    dfy_v2_status (DFY_V2_CALL *release)(dfy_v2_instance* instance, dfy_v2_tool_ticket ticket, dfy_v2_error* error);
} dfy_v2_tool_api;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
