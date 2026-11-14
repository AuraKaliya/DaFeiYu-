# 验证生产域签发器与同一内部算法的整数边界，不连接任何插件或模型服务。
add_executable(dfy_runtime_ids_tests runtime_ids.cpp)
target_link_libraries(dfy_runtime_ids_tests PRIVATE dfy_runtime dfy_build_options)
set_target_properties(dfy_runtime_ids_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/runtime_ids.cpp")

# 直接验收生产引用表，不用 SDK 隔离夹具替代槽位与代次实现。
add_executable(dfy_runtime_handles_tests runtime_handles.cpp)
target_link_libraries(dfy_runtime_handles_tests PRIVATE dfy_runtime dfy_build_options)
set_target_properties(dfy_runtime_handles_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/runtime_handles.cpp")

# 验证宿主基础值的类型、精度及生产引用表中的寿命。
add_executable(dfy_runtime_values_tests runtime_values.cpp)
target_link_libraries(dfy_runtime_values_tests PRIVATE dfy_runtime dfy_build_options)
set_target_properties(dfy_runtime_values_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/runtime_values.cpp")

# 验证最小结构描述的输入隔离、字段约束及引用寿命，不接入类型目录。
add_executable(dfy_runtime_struct_descriptors_tests runtime_struct_descriptors.cpp)
target_link_libraries(dfy_runtime_struct_descriptors_tests PRIVATE dfy_runtime dfy_build_options)
set_target_properties(dfy_runtime_struct_descriptors_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/runtime_struct_descriptors.cpp")

# 验证有界类型目录的精确查询、冲突拒绝及独立快照寿命。
add_executable(dfy_runtime_types_tests runtime_types.cpp)
target_link_libraries(dfy_runtime_types_tests PRIVATE dfy_runtime dfy_build_options)
set_target_properties(dfy_runtime_types_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/runtime_types.cpp")

# 验证结构载荷的严格构造、只读反射和描述寿命。
add_executable(dfy_runtime_struct_values_tests runtime_struct_values.cpp)
target_link_libraries(dfy_runtime_struct_values_tests PRIVATE dfy_runtime dfy_build_options)
set_target_properties(dfy_runtime_struct_values_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/runtime_struct_values.cpp")

# 验证有界执行器的延后 FIFO、重入拒绝和捕获寿命。
add_executable(dfy_runtime_executor_tests runtime_executor.cpp)
target_link_libraries(dfy_runtime_executor_tests PRIVATE dfy_runtime dfy_build_options)
set_target_properties(dfy_runtime_executor_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/runtime_executor.cpp")

# 验证宿主操作终态与客户端引用分离的工作寿命。
add_executable(dfy_runtime_operations_tests runtime_operations.cpp)
target_link_libraries(dfy_runtime_operations_tests PRIVATE dfy_runtime dfy_build_options)
set_target_properties(dfy_runtime_operations_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/runtime_operations.cpp")

# 验证通知登记、不可共享引用与独立撤销，调度另行分步加入。
add_executable(dfy_runtime_notifications_tests runtime_notifications.cpp)
target_link_libraries(dfy_runtime_notifications_tests PRIVATE dfy_runtime dfy_build_options)
set_target_properties(dfy_runtime_notifications_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/runtime_notifications.cpp")

# 验证资源所有者的登记、清理阶段及实例代次隔离。
add_executable(dfy_runtime_resources_tests runtime_resources.cpp)
target_link_libraries(dfy_runtime_resources_tests PRIVATE dfy_runtime dfy_build_options)
set_target_properties(dfy_runtime_resources_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/runtime_resources.cpp")

# 验证新事件链路的顺序接纳、有界引用及不可变载荷寿命。
add_executable(dfy_event_store_tests event_store.cpp)
target_link_libraries(dfy_event_store_tests PRIVATE dfy_event_streams dfy_build_options)
set_target_properties(dfy_event_store_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/event_store.cpp")

# 验证独立订阅队列、两种关闭及读出事件的持有责任。
add_executable(dfy_subscription_queue_tests subscription_queue.cpp)
target_link_libraries(dfy_subscription_queue_tests PRIVATE dfy_event_streams dfy_build_options)
set_target_properties(dfy_subscription_queue_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/subscription_queue.cpp")

# 验证广播接纳的原子性、接收方隔离及事件引用预算。
add_executable(dfy_broadcast_channel_tests broadcast_channel.cpp)
target_link_libraries(dfy_broadcast_channel_tests PRIVATE dfy_event_streams dfy_build_options)
set_target_properties(dfy_broadcast_channel_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/broadcast_channel.cpp")

# 验证有界读取请求、取消与流结束区分及生产 FIFO 完成通知。
add_executable(dfy_subscription_reader_tests subscription_reader.cpp)
target_link_libraries(dfy_subscription_reader_tests PRIVATE dfy_event_streams dfy_build_options)
set_target_properties(dfy_subscription_reader_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/subscription_reader.cpp")

# 配置测试直接复用正式模型库的读取逻辑。
add_executable(dfy_model_config_tests model_config.cpp)
target_link_libraries(dfy_model_config_tests PRIVATE dfy_model dfy_build_options nlohmann_json::nlohmann_json)

# 单独编译协议实现，以测试传输替代 WinHTTP，测试全程不访问模型服务。
add_executable(dfy_model_client_tests model_client.cpp "${PROJECT_SOURCE_DIR}/model/client.cpp")
target_include_directories(dfy_model_client_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/model/include" "${PROJECT_SOURCE_DIR}/platform/include")
target_link_libraries(dfy_model_client_tests PRIVATE dfy_build_options nlohmann_json::nlohmann_json)
target_compile_definitions(dfy_model_client_tests PRIVATE _CRT_SECURE_NO_WARNINGS)

# 编译真实会话、事件和终端流程，仅用测试模型替代外部服务调用。
add_executable(dfy_agent_tests agent_session.cpp
  "${PROJECT_SOURCE_DIR}/agent/session.cpp" "${PROJECT_SOURCE_DIR}/app/chat.cpp")
target_include_directories(dfy_agent_tests PRIVATE "${PROJECT_SOURCE_DIR}/agent/include"
  "${PROJECT_SOURCE_DIR}/model/include" "${PROJECT_SOURCE_DIR}/app")
target_link_libraries(dfy_agent_tests PRIVATE dfy_events dfy_build_options)

# 事件测试直接链接独立总线，不引入模型或平台传输。
add_executable(dfy_events_tests events.cpp)
target_link_libraries(dfy_events_tests PRIVATE dfy_events dfy_build_options)

# 清单测试使用真实目录和链接，只解析文件，不执行任何插件代码。
add_executable(dfy_plugin_manifest_tests plugin_manifest.cpp)
target_link_libraries(dfy_plugin_manifest_tests PRIVATE dfy_plugins dfy_build_options nlohmann_json::nlohmann_json)

# 发现测试在隔离目录中增删包，验证刷新、冲突和快照的生命周期。
add_executable(dfy_plugin_registry_tests plugin_registry.cpp)
target_link_libraries(dfy_plugin_registry_tests PRIVATE dfy_plugins dfy_build_options nlohmann_json::nlohmann_json)

# 配置测试验证默认值、独立用户覆盖和只读快照，不创建插件实例。
add_executable(dfy_plugin_config_tests plugin_config.cpp)
target_link_libraries(dfy_plugin_config_tests PRIVATE dfy_plugins dfy_build_options nlohmann_json::nlohmann_json)

# ABI 消费者仅依赖 SDK，不继承本体字符集选项；运行时按导出名查询纯 C 示例 DLL。
add_executable(dfy_sdk_tests sdk_abi.cpp)
target_link_libraries(dfy_sdk_tests PRIVATE dfy_sdk)
target_compile_definitions(dfy_sdk_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(dfy_sdk_tests dfy_query_example)

# v2 查询消费者只依赖 SDK，通过真实 C DLL 验证独立入口与能力表协商。
add_subdirectory(fixtures/query_v2)
add_executable(dfy_sdk_v2_tests sdk_abi_v2.cpp)
target_link_libraries(dfy_sdk_v2_tests PRIVATE dfy_sdk)
target_compile_definitions(dfy_sdk_v2_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(dfy_sdk_v2_tests dfy_query_v2_fixture)

# 引用契约使用隔离宿主和同一真实 C DLL 验收，不链接生产句柄运行时。
add_executable(dfy_sdk_handles_v2_tests sdk_handles_v2.cpp)
target_link_libraries(dfy_sdk_handles_v2_tests PRIVATE dfy_sdk)
target_compile_definitions(dfy_sdk_handles_v2_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(dfy_sdk_handles_v2_tests dfy_query_v2_fixture)

# 操作状态仅由隔离宿主推进，通过真实 C DLL 验证快照及引用，不接入生产执行器。
add_executable(dfy_sdk_operations_v2_tests sdk_operations_v2.cpp)
target_link_libraries(dfy_sdk_operations_v2_tests PRIVATE dfy_sdk)
target_compile_definitions(dfy_sdk_operations_v2_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(dfy_sdk_operations_v2_tests dfy_query_v2_fixture)

# 通知行为通过真实 C 回调与有界隔离宿主验证，正式执行器和资源管理仍独立建设。
add_executable(dfy_sdk_notifications_v2_tests sdk_notifications_v2.cpp)
target_link_libraries(dfy_sdk_notifications_v2_tests PRIVATE dfy_sdk)
target_compile_definitions(dfy_sdk_notifications_v2_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(dfy_sdk_notifications_v2_tests dfy_query_v2_fixture)

# 使用独立真实 DLL 夹具测试平台加载器，不接入插件实例或 ABI 查询校验。
add_subdirectory(fixtures/library)
add_executable(dfy_library_tests dynamic_library.cpp)
target_link_libraries(dfy_library_tests PRIVATE dfy_library dfy_build_options)
target_compile_definitions(dfy_library_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(dfy_library_tests dfy_loader_payload dfy_loader_x86)

# 使用真实 DLL 验证独立代码凭证与 FIFO 排队工作中的模块寿命。
add_executable(dfy_code_module_tests code_module.cpp)
target_link_libraries(dfy_code_module_tests PRIVATE dfy_library dfy_runtime dfy_build_options)
target_compile_definitions(dfy_code_module_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(dfy_code_module_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/code_module.cpp")
add_dependencies(dfy_code_module_tests dfy_loader_payload)

# 用可控真实 DLL 覆盖 ABI 查询结果及构造失败后的引用回收。
add_subdirectory(fixtures/query)
add_executable(dfy_plugin_module_tests plugin_module.cpp)
target_link_libraries(dfy_plugin_module_tests PRIVATE dfy_plugins dfy_build_options nlohmann_json::nlohmann_json)
target_compile_definitions(dfy_plugin_module_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(dfy_plugin_module_tests dfy_query_fixture dfy_query_example dfy_loader_dependency)

# 独立验证 v2 生产查询，不改变 v1 夹具或模块接口。
add_subdirectory(fixtures/query_v2_faults)
add_executable(dfy_plugin_module_v2_tests plugin_module_v2.cpp)
target_link_libraries(dfy_plugin_module_v2_tests PRIVATE dfy_plugins dfy_build_options)
target_compile_definitions(dfy_plugin_module_v2_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(dfy_plugin_module_v2_tests dfy_query_v2_faults dfy_query_example)
set_target_properties(dfy_plugin_module_v2_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_module_v2.cpp")
add_test(NAME plugins.module_v2 COMMAND dfy_plugin_module_v2_tests "$<TARGET_FILE:dfy_query_v2_faults>"
  "$<TARGET_FILE:dfy_query_example>" "${CMAKE_CURRENT_BINARY_DIR}/module-v2/$<CONFIG>")
set_tests_properties(plugins.module_v2 PROPERTIES TIMEOUT 15)

# 真实 C DLL 查询生产宿主服务，校验调用作用域与引用隔离。
add_executable(dfy_plugin_host_tests plugin_host.cpp)
target_link_libraries(dfy_plugin_host_tests PRIVATE dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_host_tests dfy_query_v2_fixture)
set_target_properties(dfy_plugin_host_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_host.cpp")
add_test(NAME plugins.host COMMAND dfy_plugin_host_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.host PROPERTIES TIMEOUT 15)

# 纯 C 消费生产读取服务，核对完整请求与载荷预算的寿命关系。
add_executable(dfy_plugin_reads_tests plugin_reads.cpp)
target_link_libraries(dfy_plugin_reads_tests PRIVATE dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_reads_tests dfy_query_v2_fixture)
set_target_properties(dfy_plugin_reads_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_reads.cpp")
add_test(NAME plugins.reads COMMAND dfy_plugin_reads_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.reads PROPERTIES TIMEOUT 15)

# 使用真实 C 回调串联生产读取、通知、资源登记和代码活动保护。
add_executable(dfy_plugin_notifications_tests plugin_notifications.cpp)
target_link_libraries(dfy_plugin_notifications_tests PRIVATE dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_notifications_tests dfy_query_v2_fixture)
set_target_properties(dfy_plugin_notifications_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_notifications.cpp")
add_test(NAME plugins.notifications COMMAND dfy_plugin_notifications_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.notifications PROPERTIES TIMEOUT 15)

# 验证 C 订阅创建和关闭的独立接收方与失败回滚。
add_executable(dfy_plugin_subscriptions_tests plugin_subscriptions.cpp)
target_link_libraries(dfy_plugin_subscriptions_tests PRIVATE dfy_plugins dfy_build_options)
set_target_properties(dfy_plugin_subscriptions_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_subscriptions.cpp")
add_test(NAME plugins.subscriptions COMMAND dfy_plugin_subscriptions_tests)
set_tests_properties(plugins.subscriptions PROPERTIES TIMEOUT 15)

# 真实 C 实例验证停止受理与停止完成分离，保持 v1 生命周期契约冻结。
add_executable(dfy_sdk_lifecycle_v2_tests sdk_lifecycle_v2.cpp)
target_link_libraries(dfy_sdk_lifecycle_v2_tests PRIVATE dfy_plugins dfy_build_options)
add_dependencies(dfy_sdk_lifecycle_v2_tests dfy_query_v2_fixture)
set_target_properties(dfy_sdk_lifecycle_v2_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/sdk_lifecycle_v2.cpp")
add_test(NAME sdk.lifecycle_v2 COMMAND dfy_sdk_lifecycle_v2_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(sdk.lifecycle_v2 PROPERTIES TIMEOUT 15)

# 真实 C 实例验收宿主控制器和停止失败后的可查询保留状态。
add_executable(dfy_plugin_instance_tests plugin_instance.cpp)
target_link_libraries(dfy_plugin_instance_tests PRIVATE dfy_plugins dfy_build_options)
target_compile_definitions(dfy_plugin_instance_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(dfy_plugin_instance_tests dfy_query_v2_fixture)
set_target_properties(dfy_plugin_instance_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_instance.cpp")
add_test(NAME plugins.instance COMMAND dfy_plugin_instance_tests "$<TARGET_FILE:dfy_query_v2_fixture>"
  "${CMAKE_CURRENT_BINARY_DIR}/instance/$<CONFIG>")
set_tests_properties(plugins.instance PROPERTIES TIMEOUT 15)

# 实际纯 C 观察插件只通过公开 ABI 消费生产事件，不链接到宿主测试。
add_executable(dfy_observer_tests observer.cpp)
target_link_libraries(dfy_observer_tests PRIVATE dfy_plugins dfy_build_options)
target_compile_definitions(dfy_observer_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(dfy_observer_tests dfy_observer)
set_target_properties(dfy_observer_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/observer.cpp")
add_test(NAME plugins.observer COMMAND dfy_observer_tests "$<TARGET_FILE_DIR:dfy_observer>" "${CMAKE_CURRENT_BINARY_DIR}/observer-data/$<CONFIG>")
set_tests_properties(plugins.observer PROPERTIES TIMEOUT 15)

# 验证有界嵌套结构与定义依赖，独立于 C ABI 的版本化扩展。
add_executable(dfy_runtime_nested_tests runtime_nested.cpp)
target_link_libraries(dfy_runtime_nested_tests PRIVATE dfy_runtime dfy_build_options)
set_target_properties(dfy_runtime_nested_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/runtime_nested.cpp")
add_test(NAME runtime.nested COMMAND dfy_runtime_nested_tests)
set_tests_properties(runtime.nested PROPERTIES TIMEOUT 15)

# 真实 C 消费者验证嵌套反射与递归别名的原始事件预算。
add_executable(dfy_plugin_nested_tests plugin_nested.cpp)
target_link_libraries(dfy_plugin_nested_tests PRIVATE dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_nested_tests dfy_query_v2_fixture)
set_target_properties(dfy_plugin_nested_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_nested.cpp")
add_test(NAME plugins.nested COMMAND dfy_plugin_nested_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.nested PROPERTIES TIMEOUT 15)

# 验证可选、可空与实际内容的独立约束。
add_executable(dfy_runtime_presence_tests runtime_presence.cpp)
target_link_libraries(dfy_runtime_presence_tests PRIVATE dfy_runtime dfy_build_options)
set_target_properties(dfy_runtime_presence_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/runtime_presence.cpp")
add_test(NAME runtime.presence COMMAND dfy_runtime_presence_tests)
set_tests_properties(runtime.presence PROPERTIES TIMEOUT 15)

# 验证标量数组的精确类型、不可变持有和两层预算。
add_executable(dfy_runtime_arrays_tests runtime_arrays.cpp)
target_link_libraries(dfy_runtime_arrays_tests PRIVATE dfy_runtime dfy_build_options)
set_target_properties(dfy_runtime_arrays_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/runtime_arrays.cpp")
add_test(NAME runtime.arrays COMMAND dfy_runtime_arrays_tests)
set_tests_properties(runtime.arrays PROPERTIES TIMEOUT 15)

# 验证 UTF-8 字符串键映射的唯一性、规范顺序及数据预算。
add_executable(dfy_runtime_maps_tests runtime_maps.cpp)
target_link_libraries(dfy_runtime_maps_tests PRIVATE dfy_runtime dfy_build_options)
set_target_properties(dfy_runtime_maps_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/runtime_maps.cpp")
add_test(NAME runtime.maps COMMAND dfy_runtime_maps_tests)
set_tests_properties(runtime.maps PROPERTIES TIMEOUT 15)

# 验证闭合枚举的精确整数、不可变定义与有界成员集。
add_executable(dfy_runtime_enums_tests runtime_enums.cpp)
target_link_libraries(dfy_runtime_enums_tests PRIVATE dfy_runtime dfy_build_options)
set_target_properties(dfy_runtime_enums_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/runtime_enums.cpp")
add_test(NAME runtime.enums COMMAND dfy_runtime_enums_tests)
set_tests_properties(runtime.enums PROPERTIES TIMEOUT 15)

# 验证版本化 C 枚举服务的精确值、借用和独立描述持有。
add_executable(dfy_plugin_enums_tests plugin_enums.cpp)
target_link_libraries(dfy_plugin_enums_tests PRIVATE dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_enums_tests dfy_query_v2_fixture)
set_target_properties(dfy_plugin_enums_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_enums.cpp")
add_test(NAME plugins.enums COMMAND dfy_plugin_enums_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.enums PROPERTIES TIMEOUT 15)

# 验证闭合联合的显式标签、分支约束及完整描述预算。
add_executable(dfy_runtime_unions_tests runtime_unions.cpp)
target_link_libraries(dfy_runtime_unions_tests PRIVATE dfy_runtime dfy_build_options)
set_target_properties(dfy_runtime_unions_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/runtime_unions.cpp")
add_test(NAME runtime.unions COMMAND dfy_runtime_unions_tests)
set_tests_properties(runtime.unions PROPERTIES TIMEOUT 15)

# 验证字段约束独立持有及已实现内容种类的统一反射。
add_executable(dfy_plugin_constraints_tests plugin_constraints.cpp)
target_link_libraries(dfy_plugin_constraints_tests PRIVATE dfy_plugins dfy_build_options)
set_target_properties(dfy_plugin_constraints_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_constraints.cpp")
add_test(NAME plugins.constraints COMMAND dfy_plugin_constraints_tests)
set_tests_properties(plugins.constraints PROPERTIES TIMEOUT 15)

# 验证联合分支反射、稳定标签、实际内容及独立引用寿命。
add_executable(dfy_plugin_unions_tests plugin_unions.cpp)
target_link_libraries(dfy_plugin_unions_tests PRIVATE dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_unions_tests dfy_query_v2_fixture)
set_target_properties(dfy_plugin_unions_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_unions.cpp")
add_test(NAME plugins.unions COMMAND dfy_plugin_unions_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.unions PROPERTIES TIMEOUT 15)

# 验证完整匿名元素约束及容器层的展开计量。
add_executable(dfy_runtime_elements_tests runtime_elements.cpp)
target_link_libraries(dfy_runtime_elements_tests PRIVATE dfy_runtime dfy_build_options)
set_target_properties(dfy_runtime_elements_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/runtime_elements.cpp")
add_test(NAME runtime.elements COMMAND dfy_runtime_elements_tests)
set_tests_properties(runtime.elements PROPERTIES TIMEOUT 15)

# 验证版本 2 组合数组的数据、匿名元素约束与叶引用寿命。
add_executable(dfy_plugin_arrays_tests plugin_arrays.cpp)
target_link_libraries(dfy_plugin_arrays_tests PRIVATE dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_arrays_tests dfy_query_v2_fixture)
set_target_properties(dfy_plugin_arrays_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_arrays.cpp")
add_test(NAME plugins.arrays COMMAND dfy_plugin_arrays_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.arrays PROPERTIES TIMEOUT 15)

# 验证版本 2 组合映射的精确键、完整值约束和数据寿命。
add_executable(dfy_plugin_maps_tests plugin_maps.cpp)
target_link_libraries(dfy_plugin_maps_tests PRIVATE dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_maps_tests dfy_query_v2_fixture)
set_target_properties(dfy_plugin_maps_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_maps.cpp")
add_test(NAME plugins.maps COMMAND dfy_plugin_maps_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.maps PROPERTIES TIMEOUT 15)

# 真实 C 消费者验证指定投递、独立宿主域与广播混合顺序。
add_executable(dfy_plugin_targets_tests plugin_targets.cpp)
target_link_libraries(dfy_plugin_targets_tests PRIVATE dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_targets_tests dfy_query_v2_fixture)
set_target_properties(dfy_plugin_targets_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_targets.cpp")
add_test(NAME plugins.targets COMMAND dfy_plugin_targets_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.targets PROPERTIES TIMEOUT 15)

# 验证消费者组的有界成员、共享领取和独立关闭语义。
add_executable(dfy_consumer_group_tests consumer_group.cpp)
target_link_libraries(dfy_consumer_group_tests PRIVATE dfy_event_streams dfy_build_options)
set_target_properties(dfy_consumer_group_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/consumer_group.cpp")
add_test(NAME events.consumer_group COMMAND dfy_consumer_group_tests)
set_tests_properties(events.consumer_group PROPERTIES TIMEOUT 15)

# 验证组成员等待、取消、退出和保留结果的独立预算。
add_executable(dfy_group_reader_tests group_reader.cpp)
target_link_libraries(dfy_group_reader_tests PRIVATE dfy_event_streams dfy_build_options)
set_target_properties(dfy_group_reader_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/group_reader.cpp")
add_test(NAME events.group_reader COMMAND dfy_group_reader_tests)
set_tests_properties(events.group_reader PROPERTIES TIMEOUT 15)

# 通过真实 C 读取服务验证组成员控制器和跨宿主独立停止。
add_executable(dfy_plugin_group_reads_tests plugin_group_reads.cpp)
target_link_libraries(dfy_plugin_group_reads_tests PRIVATE dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_group_reads_tests dfy_query_v2_fixture)
set_target_properties(dfy_plugin_group_reads_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_group_reads.cpp")
add_test(NAME plugins.group_reads COMMAND dfy_plugin_group_reads_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.group_reads PROPERTIES TIMEOUT 15)

# 验证组能力的条件授权、加入事务、退出和跨宿主边界。
add_executable(dfy_plugin_groups_tests plugin_groups.cpp)
target_link_libraries(dfy_plugin_groups_tests PRIVATE dfy_plugins dfy_build_options)
set_target_properties(dfy_plugin_groups_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_groups.cpp")
add_test(NAME plugins.groups COMMAND dfy_plugin_groups_tests)
set_tests_properties(plugins.groups PROPERTIES TIMEOUT 15)

# 真实 C 回调验证组成员退出与代码活动保护、排队撤销的独立边界。
add_executable(dfy_plugin_group_notifications_tests plugin_group_notifications.cpp)
target_link_libraries(dfy_plugin_group_notifications_tests PRIVATE dfy_plugins dfy_build_options)
add_dependencies(dfy_plugin_group_notifications_tests dfy_query_v2_fixture)
set_target_properties(dfy_plugin_group_notifications_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_group_notifications.cpp")
add_test(NAME plugins.group_notifications COMMAND dfy_plugin_group_notifications_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.group_notifications PROPERTIES TIMEOUT 15)

# 验证所有者权重、前置执行资格及活动任务容量。
add_executable(dfy_owner_scheduler_tests owner_scheduler.cpp)
target_link_libraries(dfy_owner_scheduler_tests PRIVATE dfy_runtime dfy_build_options)
set_target_properties(dfy_owner_scheduler_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/owner_scheduler.cpp")
add_test(NAME runtime.owner_scheduler COMMAND dfy_owner_scheduler_tests)
set_tests_properties(runtime.owner_scheduler PROPERTIES TIMEOUT 15)

# 验证真实事件共享存储及跨处理队列的所有者预算。
add_executable(dfy_event_budget_tests event_budget.cpp)
target_link_libraries(dfy_event_budget_tests PRIVATE dfy_event_streams dfy_build_options)
set_target_properties(dfy_event_budget_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/event_budget.cpp")
add_test(NAME events.budget COMMAND dfy_event_budget_tests)
set_tests_properties(events.budget PROPERTIES TIMEOUT 15)

# 验证分层条目与字节预算的原子计费、移动归还及极限。
add_executable(dfy_budget_tests budget.cpp)
target_link_libraries(dfy_budget_tests PRIVATE dfy_runtime dfy_build_options)
set_target_properties(dfy_budget_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/budget.cpp")
add_test(NAME runtime.budget COMMAND dfy_budget_tests)
set_tests_properties(runtime.budget PROPERTIES TIMEOUT 15)

# 验证真实纯 C DLL 的分步处理与跨配置数据、代码寿命。
add_executable(dfy_plugin_processing_dll_tests plugin_processing_dll.cpp)
target_link_libraries(dfy_plugin_processing_dll_tests PRIVATE dfy_plugins dfy_build_options)
set_target_properties(dfy_plugin_processing_dll_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_processing_dll.cpp")
add_test(NAME plugins.processing_dll COMMAND dfy_plugin_processing_dll_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
set_tests_properties(plugins.processing_dll PROPERTIES TIMEOUT 15)

# 验证宿主处理能力的顺序、结算及忙碌停止边界。
add_executable(dfy_plugin_processing_tests plugin_processing.cpp)
target_link_libraries(dfy_plugin_processing_tests PRIVATE dfy_plugins dfy_build_options)
set_target_properties(dfy_plugin_processing_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugin_processing.cpp")
add_test(NAME plugins.processing COMMAND dfy_plugin_processing_tests)
set_tests_properties(plugins.processing PROPERTIES TIMEOUT 15)

# 验证真实事件处理的业务结果、实际退出和独立数据寿命。
add_executable(dfy_processing_tests processing.cpp)
target_link_libraries(dfy_processing_tests PRIVATE dfy_event_streams dfy_build_options)
set_target_properties(dfy_processing_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/processing.cpp")
add_test(NAME events.processing COMMAND dfy_processing_tests)
set_tests_properties(events.processing PROPERTIES TIMEOUT 15)

# 验证顺序域的 FIFO 授予、跨等待占用及独立取消关闭。
add_executable(dfy_serial_domain_tests serial_domain.cpp)
target_link_libraries(dfy_serial_domain_tests PRIVATE dfy_runtime dfy_build_options)
set_target_properties(dfy_serial_domain_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/serial_domain.cpp")
add_test(NAME runtime.serial_domain COMMAND dfy_serial_domain_tests)
set_tests_properties(runtime.serial_domain PROPERTIES TIMEOUT 15)

# 验证顺序域显式等待依赖的同域拒绝、跨域环和登记预算。
add_executable(dfy_serial_wait_graph_tests serial_wait_graph.cpp)
target_link_libraries(dfy_serial_wait_graph_tests PRIVATE dfy_runtime dfy_build_options)
set_target_properties(dfy_serial_wait_graph_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/serial_wait_graph.cpp")
add_test(NAME runtime.serial_wait_graph COMMAND dfy_serial_wait_graph_tests)
set_tests_properties(runtime.serial_wait_graph PROPERTIES TIMEOUT 15)

# 组织 IDE 展示：测试自身源码按本目录排列，借用的业务源码保留来源模块路径。
set_target_properties(dfy_model_config_tests dfy_model_client_tests dfy_agent_tests dfy_events_tests dfy_plugin_manifest_tests
  dfy_plugin_registry_tests dfy_plugin_config_tests dfy_sdk_tests dfy_sdk_v2_tests dfy_sdk_handles_v2_tests
  dfy_sdk_operations_v2_tests dfy_sdk_notifications_v2_tests dfy_library_tests dfy_plugin_module_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/model_config.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/model_client.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/agent_session.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/events.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/plugin_manifest.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/plugin_registry.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/plugin_config.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/sdk_abi.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/dynamic_library.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/plugin_module.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk_abi_v2.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/sdk_handles_v2.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk_operations_v2.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/sdk_notifications_v2.cpp")
source_group(TREE "${PROJECT_SOURCE_DIR}" FILES
  "${PROJECT_SOURCE_DIR}/model/client.cpp" "${PROJECT_SOURCE_DIR}/agent/session.cpp"
  "${PROJECT_SOURCE_DIR}/app/chat.cpp")

# 注册 CTest：配置测试使用隔离工作目录，全部测试统一设置执行时限。
add_test(NAME runtime.identifiers COMMAND dfy_runtime_ids_tests)
set_tests_properties(runtime.identifiers PROPERTIES TIMEOUT 15)
add_test(NAME runtime.handles COMMAND dfy_runtime_handles_tests)
set_tests_properties(runtime.handles PROPERTIES TIMEOUT 15)
add_test(NAME runtime.values COMMAND dfy_runtime_values_tests)
set_tests_properties(runtime.values PROPERTIES TIMEOUT 15)
add_test(NAME runtime.struct_descriptors COMMAND dfy_runtime_struct_descriptors_tests)
set_tests_properties(runtime.struct_descriptors PROPERTIES TIMEOUT 15)
add_test(NAME runtime.types COMMAND dfy_runtime_types_tests)
set_tests_properties(runtime.types PROPERTIES TIMEOUT 15)
add_test(NAME runtime.struct_values COMMAND dfy_runtime_struct_values_tests)
set_tests_properties(runtime.struct_values PROPERTIES TIMEOUT 15)
add_test(NAME runtime.executor COMMAND dfy_runtime_executor_tests)
set_tests_properties(runtime.executor PROPERTIES TIMEOUT 15)
add_test(NAME runtime.operations COMMAND dfy_runtime_operations_tests)
set_tests_properties(runtime.operations PROPERTIES TIMEOUT 15)
add_test(NAME runtime.notifications COMMAND dfy_runtime_notifications_tests)
set_tests_properties(runtime.notifications PROPERTIES TIMEOUT 15)
add_test(NAME runtime.resources COMMAND dfy_runtime_resources_tests)
set_tests_properties(runtime.resources PROPERTIES TIMEOUT 15)
add_test(NAME events.store COMMAND dfy_event_store_tests)
set_tests_properties(events.store PROPERTIES TIMEOUT 15)
add_test(NAME events.subscription_queue COMMAND dfy_subscription_queue_tests)
set_tests_properties(events.subscription_queue PROPERTIES TIMEOUT 15)
add_test(NAME events.broadcast COMMAND dfy_broadcast_channel_tests)
set_tests_properties(events.broadcast PROPERTIES TIMEOUT 15)
add_test(NAME events.subscription_reader COMMAND dfy_subscription_reader_tests)
set_tests_properties(events.subscription_reader PROPERTIES TIMEOUT 15)
add_test(NAME model.config COMMAND dfy_model_config_tests
  "${PROJECT_SOURCE_DIR}/config" "${CMAKE_CURRENT_BINARY_DIR}/work/$<CONFIG>")
add_test(NAME model.complete COMMAND dfy_model_client_tests)
add_test(NAME agent.session COMMAND dfy_agent_tests)
add_test(NAME events.bus COMMAND dfy_events_tests)
add_test(NAME plugins.manifest COMMAND dfy_plugin_manifest_tests "${CMAKE_CURRENT_BINARY_DIR}/manifest/$<CONFIG>")
add_test(NAME plugins.registry COMMAND dfy_plugin_registry_tests "${CMAKE_CURRENT_BINARY_DIR}/registry/$<CONFIG>")
add_test(NAME plugins.config COMMAND dfy_plugin_config_tests "${CMAKE_CURRENT_BINARY_DIR}/plugin-config/$<CONFIG>")
add_test(NAME sdk.abi COMMAND dfy_sdk_tests "$<TARGET_FILE:dfy_query_example>")
add_test(NAME sdk.query_v2 COMMAND dfy_sdk_v2_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
add_test(NAME sdk.handles_v2 COMMAND dfy_sdk_handles_v2_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
add_test(NAME sdk.operations_v2 COMMAND dfy_sdk_operations_v2_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
add_test(NAME sdk.notifications_v2 COMMAND dfy_sdk_notifications_v2_tests "$<TARGET_FILE:dfy_query_v2_fixture>")
add_test(NAME platform.library COMMAND dfy_library_tests "$<TARGET_FILE:dfy_loader_dependency>"
  "$<TARGET_FILE:dfy_loader_payload>" "${DFY_LOADER_X86_DLL}" "${CMAKE_CURRENT_BINARY_DIR}/library/$<CONFIG>")
add_test(NAME platform.code_module COMMAND dfy_code_module_tests "$<TARGET_FILE:dfy_loader_payload>")
set_tests_properties(platform.code_module PROPERTIES TIMEOUT 15)
add_test(NAME plugins.module COMMAND dfy_plugin_module_tests "$<TARGET_FILE:dfy_query_fixture>"
  "$<TARGET_FILE:dfy_query_example>" "$<TARGET_FILE:dfy_loader_dependency>" "${CMAKE_CURRENT_BINARY_DIR}/module/$<CONFIG>")
set_tests_properties(model.config model.complete agent.session events.bus plugins.manifest plugins.registry plugins.config sdk.abi
  sdk.query_v2 sdk.handles_v2 sdk.operations_v2 sdk.notifications_v2 platform.library plugins.module PROPERTIES TIMEOUT 15)
