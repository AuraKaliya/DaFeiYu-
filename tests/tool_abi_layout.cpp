#include <dafeiyu/tool_abi_v2.h>
#include <dafeiyu/companion_abi_v2.h>
#include <cstddef>
#include <iostream>
#include <type_traits>
static_assert(std::is_standard_layout_v<dfy_v2_tool_api> && std::is_trivially_copyable_v<dfy_v2_tool_progress>);
static_assert(sizeof(dfy_v2_tool_ticket) == 16 && alignof(dfy_v2_tool_ticket) == 8);
static_assert(sizeof(dfy_v2_tool_description) == 64 && offsetof(dfy_v2_tool_description, id) == 16);
static_assert(sizeof(dfy_v2_tool_call) == 80 && offsetof(dfy_v2_tool_call, arguments) == 64);
static_assert(sizeof(dfy_v2_tool_progress) == 40 && offsetof(dfy_v2_tool_progress, output) == 24);
static_assert(sizeof(dfy_v2_tool_api) == 56 && offsetof(dfy_v2_tool_api, release) == 48);
static_assert(sizeof(dfy_v2_companion_description) == 40 && offsetof(dfy_v2_companion_description, state_topic) == 24);
static_assert(sizeof(dfy_v2_companion_api) == 16 && offsetof(dfy_v2_companion_api, describe) == 8);
static_assert(sizeof(dfy_v2_companion_routes_api) == 16 && offsetof(dfy_v2_companion_routes_api, resolve) == 8);
// 在独立 C 消费者中往返票据，不依赖本体 C++ 运行时类型。
extern "C" dfy_v2_tool_ticket DFY_V2_CALL dfy_test_tool_ticket(dfy_v2_tool_ticket value);
// 验证 C 与 C++ 对票据的宽整数传递，布局同时由两种语言的静态断言约束。
int main() {
    const dfy_v2_tool_ticket expected{UINT64_C(0x123456789abcdef0),UINT64_C(0xfedcba9876543210)};
    const auto result = dfy_test_tool_ticket(expected);
    if (result.owner != expected.owner || result.sequence != expected.sequence) return 1;
    std::cout << "Tool C ABI layout checks passed\n"; return 0;
}
