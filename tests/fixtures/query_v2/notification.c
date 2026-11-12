#include "api.h"

// 在真实 C 回调中检查活动撤销和借用保留，并可登记下一轮通知。
// 步骤：记录调用 → 请求自撤销并保留操作 → 按测试上下文登记子通知。
static void DFY_V2_CALL completed(void* user, dfy_v2_handle operation) {
    dfy_test_notify_context* context = (dfy_test_notify_context*)user;
    if (!context) return;
    ++context->calls;
    if (context->probe) context->probe(context->probe_user);
    if (context->sequence) context->observed_order = ++*context->sequence;
    context->revoke_status = context->notifications->revoke(context->registration, 0);
    context->retain_status = context->references->retain(operation, &context->retained, 0);
    if (context->child) {
        context->spawn_status = context->notifications->subscribe(operation, completed, context->child,
            &context->child->registration, 0);
    }
}
const dfy_test_notify_api dfy_test_notify = {{sizeof(dfy_test_notify_api), 1}, completed};
