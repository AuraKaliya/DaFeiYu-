#include <stddef.h>
#pragma pack(push, 1)
#include <dafeiyu/tool_abi_v2.h>
#include <dafeiyu/companion_abi_v2.h>
struct tool_packing_probe { char tag; uint64_t value; };
#pragma pack(pop)
_Static_assert(sizeof(struct tool_packing_probe) == 9, "caller packing restored");
_Static_assert(sizeof(dfy_v2_tool_ticket) == 16 && _Alignof(dfy_v2_tool_ticket) == 8, "ticket layout");
_Static_assert(sizeof(dfy_v2_tool_description) == 64 && offsetof(dfy_v2_tool_description, id) == 16, "description layout");
_Static_assert(sizeof(dfy_v2_tool_call) == 80 && offsetof(dfy_v2_tool_call, arguments) == 64, "call layout");
_Static_assert(sizeof(dfy_v2_tool_progress) == 40 && offsetof(dfy_v2_tool_progress, output) == 24, "progress layout");
_Static_assert(sizeof(dfy_v2_tool_api) == 56 && offsetof(dfy_v2_tool_api, release) == 48, "table layout");
_Static_assert(sizeof(dfy_v2_companion_description) == 40 && offsetof(dfy_v2_companion_description, state_topic) == 24, "companion layout");
_Static_assert(sizeof(dfy_v2_companion_api) == 16 && offsetof(dfy_v2_companion_api, describe) == 8, "companion table");
_Static_assert(sizeof(dfy_v2_companion_routes_api) == 16 && offsetof(dfy_v2_companion_routes_api, resolve) == 8, "role routes");
// 从纯 C 消费者按值返回工作票据，验证宽整数与调用约定。
dfy_v2_tool_ticket DFY_V2_CALL dfy_test_tool_ticket(dfy_v2_tool_ticket value) { return value; }
