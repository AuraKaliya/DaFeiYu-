dfy_test_executable(dfy_plugin_tools_tests plugin_tools.cpp)
target_link_libraries(dfy_plugin_tools_tests PRIVATE dfy_plugin_tools dfy_build_options)
add_dependencies(dfy_plugin_tools_tests dfy_tools_fixture dfy_query_v2_fixture)
add_test(NAME plugins.tools COMMAND dfy_plugin_tools_tests "$<TARGET_FILE_DIR:dfy_tools_fixture>" "${PROJECT_BINARY_DIR}/tests/tools-data/$<CONFIG>" "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.tools PROPERTIES TIMEOUT 15)
dfy_test_executable(dfy_plugin_tool_work_tests plugin_tool_work.cpp)
target_link_libraries(dfy_plugin_tool_work_tests PRIVATE dfy_plugin_tools dfy_build_options)
add_dependencies(dfy_plugin_tool_work_tests dfy_tools_fixture)
add_test(NAME plugins.tool_work COMMAND dfy_plugin_tool_work_tests "$<TARGET_FILE_DIR:dfy_tools_fixture>" "${PROJECT_BINARY_DIR}/tests/tool-work-data/$<CONFIG>")
set_tests_properties(plugins.tool_work PROPERTIES TIMEOUT 15)
dfy_test_executable(dfy_plugin_tool_abi_tests plugin_tool_abi.cpp)
target_link_libraries(dfy_plugin_tool_abi_tests PRIVATE dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_tool_abi_tests dfy_tools_fixture)
add_test(NAME plugins.tool_abi COMMAND dfy_plugin_tool_abi_tests "$<TARGET_FILE:dfy_tools_fixture>" "${PROJECT_BINARY_DIR}/tests/tool-abi-data/$<CONFIG>")
set_tests_properties(plugins.tool_abi PROPERTIES TIMEOUT 15)

# v2 查询消费者只依赖 SDK，通过真实 C DLL 验证独立入口与能力表协商。

# 使用独立真实 DLL 夹具测试平台加载器，不接入插件实例或 ABI 查询校验。

# 用可控真实 DLL 覆盖 ABI 查询结果及构造失败后的引用回收。
dfy_test_executable(dfy_plugin_module_tests plugin_module.cpp)
target_link_libraries(dfy_plugin_module_tests PRIVATE dfy_plugins dfy_build_options nlohmann_json::nlohmann_json)
target_compile_definitions(dfy_plugin_module_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(dfy_plugin_module_tests dfy_query_fixture dfy_query_example dfy_loader_dependency)

# 独立验证 v2 生产查询，不改变 v1 夹具或模块接口。
dfy_test_executable(dfy_plugin_module_v2_tests plugin_module_v2.cpp)
target_link_libraries(dfy_plugin_module_v2_tests PRIVATE dfy_plugins dfy_build_options)
target_compile_definitions(dfy_plugin_module_v2_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(dfy_plugin_module_v2_tests dfy_query_v2_faults dfy_query_example)
add_test(NAME plugins.module_v2 COMMAND dfy_plugin_module_v2_tests "$<TARGET_FILE:dfy_query_v2_faults>"
  "$<TARGET_FILE:dfy_query_example>" "${PROJECT_BINARY_DIR}/tests/module-v2/$<CONFIG>")
set_tests_properties(plugins.module_v2 PROPERTIES TIMEOUT 15)

# 真实 C DLL 查询生产宿主服务，校验调用作用域与引用隔离。
dfy_test_executable(dfy_plugin_host_tests plugin_host.cpp)
target_link_libraries(dfy_plugin_host_tests PRIVATE dfy_sdk_test_headers dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_host_tests dfy_query_v2_fixture)
add_test(NAME plugins.host COMMAND dfy_plugin_host_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.host PROPERTIES TIMEOUT 15)

# 纯 C 消费生产读取服务，核对完整请求与载荷预算的寿命关系。
dfy_test_executable(dfy_plugin_reads_tests plugin_reads.cpp)
target_link_libraries(dfy_plugin_reads_tests PRIVATE dfy_sdk_test_headers dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_reads_tests dfy_query_v2_fixture)
add_test(NAME plugins.reads COMMAND dfy_plugin_reads_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.reads PROPERTIES TIMEOUT 15)

# 使用真实 C 回调串联生产读取、通知、资源登记和代码活动保护。
dfy_test_executable(dfy_plugin_notifications_tests plugin_notifications.cpp)
target_link_libraries(dfy_plugin_notifications_tests PRIVATE dfy_sdk_test_headers dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_notifications_tests dfy_query_v2_fixture)
add_test(NAME plugins.notifications COMMAND dfy_plugin_notifications_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.notifications PROPERTIES TIMEOUT 15)

dfy_test_executable(dfy_plugin_mailbox_dll_tests plugin_mailbox_dll.cpp)
target_link_libraries(dfy_plugin_mailbox_dll_tests PRIVATE dfy_sdk_test_headers dfy_management dfy_build_options)
add_dependencies(dfy_plugin_mailbox_dll_tests dfy_mailbox_fixture dfy_status_example)
add_test(NAME plugins.mailbox_dll COMMAND dfy_plugin_mailbox_dll_tests "$<TARGET_FILE:dfy_mailbox_fixture>"
  "${PROJECT_BINARY_DIR}/tests/mailbox-dll" "$<TARGET_FILE_DIR:dfy_status_example>")

dfy_test_executable(dfy_management_mailbox_tests management_mailboxes.cpp)
target_link_libraries(dfy_management_mailbox_tests PRIVATE dfy_sdk_test_headers dfy_management dfy_build_options)
add_dependencies(dfy_management_mailbox_tests dfy_mailbox_fixture)
add_test(NAME app.management_mailboxes COMMAND dfy_management_mailbox_tests "$<TARGET_FILE:dfy_mailbox_fixture>"
  "${CMAKE_CURRENT_BINARY_DIR}/mailbox-management")

dfy_test_executable(dfy_management_plugins_tests management_plugins.cpp)
target_link_libraries(dfy_management_plugins_tests PRIVATE dfy_sdk_test_headers dfy_management dfy_build_options)
add_dependencies(dfy_management_plugins_tests dfy_query_v2_fixture dfy_mailbox_fixture dfy_public_settings_fixture)
add_test(NAME app.management_plugins COMMAND dfy_management_plugins_tests "$<TARGET_FILE:dfy_query_v2_fixture>"
  "${CMAKE_CURRENT_BINARY_DIR}/plugin-management" "$<TARGET_FILE:dfy_mailbox_fixture>")
get_target_property(public_settings_package dfy_public_settings_fixture DFY_FIXTURE_PACKAGE)
add_test(NAME app.management_plugin_settings COMMAND dfy_management_plugins_tests "$<TARGET_FILE:dfy_query_v2_fixture>"
  "${CMAKE_CURRENT_BINARY_DIR}/public-plugin-settings/$<CONFIG>" --public-settings "${public_settings_package}")
set_tests_properties(app.management_plugin_settings PROPERTIES TIMEOUT 90)

dfy_test_executable(dfy_management_plugin_recovery_tests management_plugin_recovery.cpp)
target_link_libraries(dfy_management_plugin_recovery_tests PRIVATE dfy_sdk_test_headers dfy_management dfy_build_options)
add_dependencies(dfy_management_plugin_recovery_tests dfy_query_v2_fixture)
add_test(NAME app.management_plugin_recovery COMMAND dfy_management_plugin_recovery_tests "$<TARGET_FILE:dfy_query_v2_fixture>"
  "${CMAKE_CURRENT_BINARY_DIR}/plugin-recovery/$<CONFIG>")
set_tests_properties(app.management_plugin_recovery PROPERTIES TIMEOUT 90)

# 真实 C 实例验证停止受理与停止完成分离，保持 v1 生命周期契约冻结。
dfy_test_executable(dfy_sdk_lifecycle_v2_tests sdk_lifecycle_v2.cpp)
target_link_libraries(dfy_sdk_lifecycle_v2_tests PRIVATE dfy_plugins dfy_build_options)
add_dependencies(dfy_sdk_lifecycle_v2_tests dfy_query_v2_fixture)
add_test(NAME sdk.lifecycle_v2 COMMAND dfy_sdk_lifecycle_v2_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(sdk.lifecycle_v2 PROPERTIES TIMEOUT 15)

# 真实 C 实例验收宿主控制器和停止失败后的可查询保留状态。
dfy_test_executable(dfy_plugin_instance_tests plugin_instance.cpp)
target_link_libraries(dfy_plugin_instance_tests PRIVATE dfy_sdk_test_headers dfy_plugins dfy_build_options)
target_compile_definitions(dfy_plugin_instance_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(dfy_plugin_instance_tests dfy_query_v2_fixture)
add_test(NAME plugins.instance COMMAND dfy_plugin_instance_tests "$<TARGET_FILE:dfy_query_v2_fixture>"
  "${PROJECT_BINARY_DIR}/tests/instance/$<CONFIG>")
set_tests_properties(plugins.instance PROPERTIES TIMEOUT 15)

# 实际纯 C 观察插件只通过公开 ABI 消费生产事件，不链接到宿主测试。
dfy_test_executable(dfy_observer_tests observer.cpp)
target_link_libraries(dfy_observer_tests PRIVATE dfy_plugins dfy_build_options)
target_compile_definitions(dfy_observer_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(dfy_observer_tests dfy_observer)
add_test(NAME plugins.observer COMMAND dfy_observer_tests "$<TARGET_FILE_DIR:dfy_observer>" "${PROJECT_BINARY_DIR}/tests/observer-data/$<CONFIG>")
set_tests_properties(plugins.observer PROPERTIES TIMEOUT 15)

# 真实 C 消费者验证嵌套反射与递归别名的原始事件预算。
dfy_test_executable(dfy_plugin_nested_tests plugin_nested.cpp)
target_link_libraries(dfy_plugin_nested_tests PRIVATE dfy_sdk_test_headers dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_nested_tests dfy_query_v2_fixture)
add_test(NAME plugins.nested COMMAND dfy_plugin_nested_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.nested PROPERTIES TIMEOUT 15)

# 验证版本化 C 枚举服务的精确值、借用和独立描述持有。
dfy_test_executable(dfy_plugin_enums_tests plugin_enums.cpp)
target_link_libraries(dfy_plugin_enums_tests PRIVATE dfy_sdk_test_headers dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_enums_tests dfy_query_v2_fixture)
add_test(NAME plugins.enums COMMAND dfy_plugin_enums_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.enums PROPERTIES TIMEOUT 15)

# 验证联合分支反射、稳定标签、实际内容及独立引用寿命。
dfy_test_executable(dfy_plugin_unions_tests plugin_unions.cpp)
target_link_libraries(dfy_plugin_unions_tests PRIVATE dfy_sdk_test_headers dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_unions_tests dfy_query_v2_fixture)
add_test(NAME plugins.unions COMMAND dfy_plugin_unions_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.unions PROPERTIES TIMEOUT 15)

# 验证版本 2 组合数组的数据、匿名元素约束与叶引用寿命。
dfy_test_executable(dfy_plugin_arrays_tests plugin_arrays.cpp)
target_link_libraries(dfy_plugin_arrays_tests PRIVATE dfy_sdk_test_headers dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_arrays_tests dfy_query_v2_fixture)
add_test(NAME plugins.arrays COMMAND dfy_plugin_arrays_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.arrays PROPERTIES TIMEOUT 15)

# 验证版本 2 组合映射的精确键、完整值约束和数据寿命。
dfy_test_executable(dfy_plugin_maps_tests plugin_maps.cpp)
target_link_libraries(dfy_plugin_maps_tests PRIVATE dfy_sdk_test_headers dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_maps_tests dfy_query_v2_fixture)
add_test(NAME plugins.maps COMMAND dfy_plugin_maps_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.maps PROPERTIES TIMEOUT 15)

# 真实 C 消费者验证指定投递、独立宿主域与广播混合顺序。
dfy_test_executable(dfy_plugin_targets_tests plugin_targets.cpp)
target_link_libraries(dfy_plugin_targets_tests PRIVATE dfy_sdk_test_headers dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_targets_tests dfy_query_v2_fixture)
add_test(NAME plugins.targets COMMAND dfy_plugin_targets_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.targets PROPERTIES TIMEOUT 15)

# 通过真实 C 读取服务验证组成员控制器和跨宿主独立停止。
dfy_test_executable(dfy_plugin_group_reads_tests plugin_group_reads.cpp)
target_link_libraries(dfy_plugin_group_reads_tests PRIVATE dfy_sdk_test_headers dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_group_reads_tests dfy_query_v2_fixture)
add_test(NAME plugins.group_reads COMMAND dfy_plugin_group_reads_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.group_reads PROPERTIES TIMEOUT 15)

# 真实 C 回调验证组成员退出与代码活动保护、排队撤销的独立边界。
dfy_test_executable(dfy_plugin_group_notifications_tests plugin_group_notifications.cpp)
target_link_libraries(dfy_plugin_group_notifications_tests PRIVATE dfy_sdk_test_headers dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_group_notifications_tests dfy_query_v2_fixture)
add_test(NAME plugins.group_notifications COMMAND dfy_plugin_group_notifications_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.group_notifications PROPERTIES TIMEOUT 15)

# 验证卸载后移除、用户配置保持及私有清理残留重试。
dfy_test_executable(dfy_package_remove_tests package_remove.cpp)
add_dependencies(dfy_package_remove_tests dfy_observer)
target_link_libraries(dfy_package_remove_tests PRIVATE dfy_plugins dfy_build_options)
target_compile_definitions(dfy_package_remove_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_test(NAME plugins.remove COMMAND dfy_package_remove_tests "$<TARGET_FILE_DIR:dfy_observer>" "${PROJECT_BINARY_DIR}/tests/remove-data/$<CONFIG>")
set_tests_properties(plugins.remove PROPERTIES TIMEOUT 15)

# 验证本地包暂存、默认配置复核、同名拒绝和发布回滚。
dfy_test_executable(dfy_package_install_tests package_install.cpp)
add_dependencies(dfy_package_install_tests dfy_observer)
target_link_libraries(dfy_package_install_tests PRIVATE dfy_plugin_host_test_support dfy_plugins dfy_build_options)
target_compile_definitions(dfy_package_install_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_test(NAME plugins.install COMMAND dfy_package_install_tests "$<TARGET_FILE_DIR:dfy_observer>" "${PROJECT_BINARY_DIR}/tests/install-data/$<CONFIG>")
set_tests_properties(plugins.install PROPERTIES TIMEOUT 15)

# 验证管理器发现、加载、消费、卸载及失败状态整体保留。
dfy_test_executable(dfy_plugin_manager_tests plugin_manager.cpp)
add_dependencies(dfy_plugin_manager_tests dfy_observer dfy_query_v2_fixture)
target_link_libraries(dfy_plugin_manager_tests PRIVATE dfy_plugins dfy_build_options)
add_test(NAME plugins.manager COMMAND dfy_plugin_manager_tests "$<TARGET_FILE_DIR:dfy_observer>" "$<TARGET_FILE:dfy_query_v2_fixture>" "${PROJECT_BINARY_DIR}/tests/manager-data/$<CONFIG>")
set_tests_properties(plugins.manager PROPERTIES TIMEOUT 15)

# 验证真实插件实例依赖、环检查及旧绑定代码寿命。
dfy_test_executable(dfy_plugin_dependency_tests plugin_dependencies.cpp)
add_dependencies(dfy_plugin_dependency_tests dfy_observer)
target_link_libraries(dfy_plugin_dependency_tests PRIVATE dfy_plugins dfy_build_options)
add_test(NAME plugins.dependencies COMMAND dfy_plugin_dependency_tests "$<TARGET_FILE_DIR:dfy_observer>" "${PROJECT_BINARY_DIR}/tests/dependency-data/$<CONFIG>")
set_tests_properties(plugins.dependencies PROPERTIES TIMEOUT 15)

# 验证真实纯 C DLL 的分步处理与跨配置数据、代码寿命。
dfy_test_executable(dfy_plugin_processing_dll_tests plugin_processing_dll.cpp)
add_dependencies(dfy_plugin_processing_dll_tests dfy_query_v2_fixture)
target_link_libraries(dfy_plugin_processing_dll_tests PRIVATE dfy_sdk_test_headers dfy_plugins dfy_build_options)
add_test(NAME plugins.processing_dll COMMAND dfy_plugin_processing_dll_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.processing_dll PROPERTIES TIMEOUT 15)

add_test(NAME plugins.module COMMAND dfy_plugin_module_tests "$<TARGET_FILE:dfy_query_fixture>"
  "$<TARGET_FILE:dfy_query_example>" "$<TARGET_FILE:dfy_loader_dependency>" "${PROJECT_BINARY_DIR}/tests/module/$<CONFIG>")
set_tests_properties(plugins.module PROPERTIES TIMEOUT 15)
