# 跨模块场景通过测试接口使用本模块的夹具头文件。
add_library(dfy_sdk_test_headers INTERFACE)
target_include_directories(dfy_sdk_test_headers INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")

add_subdirectory(fixtures)

dfy_test_executable(dfy_tool_abi_tests tool_abi_layout.cpp tool_abi_layout.c)
target_link_libraries(dfy_tool_abi_tests PRIVATE dfy_sdk)
target_compile_features(dfy_tool_abi_tests PRIVATE c_std_11 cxx_std_20)
add_test(NAME sdk.tool_abi COMMAND dfy_tool_abi_tests)
set_tests_properties(sdk.tool_abi PROPERTIES TIMEOUT 15)

# ABI 消费者仅依赖 SDK，不继承本体字符集选项；运行时按导出名查询纯 C 示例 DLL。
dfy_test_executable(dfy_sdk_tests sdk_abi.cpp)
target_link_libraries(dfy_sdk_tests PRIVATE dfy_sdk)
target_compile_definitions(dfy_sdk_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(dfy_sdk_tests dfy_query_example)
add_test(NAME sdk.abi COMMAND dfy_sdk_tests "$<TARGET_FILE:dfy_query_example>")
set_tests_properties(sdk.abi PROPERTIES TIMEOUT 15)

dfy_test_executable(dfy_sdk_v2_tests sdk_abi_v2.cpp)
target_link_libraries(dfy_sdk_v2_tests PRIVATE dfy_sdk)
target_compile_definitions(dfy_sdk_v2_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(dfy_sdk_v2_tests dfy_query_v2_fixture)
add_test(NAME sdk.query_v2 COMMAND dfy_sdk_v2_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(sdk.query_v2 PROPERTIES TIMEOUT 15)

# 引用契约使用隔离宿主和同一真实 C DLL 验收，不链接生产句柄运行时。
dfy_test_executable(dfy_sdk_handles_v2_tests sdk_handles_v2.cpp)
target_link_libraries(dfy_sdk_handles_v2_tests PRIVATE dfy_sdk)
target_compile_definitions(dfy_sdk_handles_v2_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(dfy_sdk_handles_v2_tests dfy_query_v2_fixture)
add_test(NAME sdk.handles_v2 COMMAND dfy_sdk_handles_v2_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(sdk.handles_v2 PROPERTIES TIMEOUT 15)

# 操作状态仅由隔离宿主推进，通过真实 C DLL 验证快照及引用，不接入生产执行器。
dfy_test_executable(dfy_sdk_operations_v2_tests sdk_operations_v2.cpp)
target_link_libraries(dfy_sdk_operations_v2_tests PRIVATE dfy_sdk)
target_compile_definitions(dfy_sdk_operations_v2_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(dfy_sdk_operations_v2_tests dfy_query_v2_fixture)
add_test(NAME sdk.operations_v2 COMMAND dfy_sdk_operations_v2_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(sdk.operations_v2 PROPERTIES TIMEOUT 15)

# 通知行为通过真实 C 回调与有界隔离宿主验证，正式执行器和资源管理仍独立建设。
dfy_test_executable(dfy_sdk_notifications_v2_tests sdk_notifications_v2.cpp)
target_link_libraries(dfy_sdk_notifications_v2_tests PRIVATE dfy_sdk)
target_compile_definitions(dfy_sdk_notifications_v2_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(dfy_sdk_notifications_v2_tests dfy_query_v2_fixture)
add_test(NAME sdk.notifications_v2 COMMAND dfy_sdk_notifications_v2_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(sdk.notifications_v2 PROPERTIES TIMEOUT 15)
