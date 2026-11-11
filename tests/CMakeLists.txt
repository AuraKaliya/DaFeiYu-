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

# 用可控真实 DLL 覆盖 ABI 查询结果及构造失败后的引用回收。
add_subdirectory(fixtures/query)
add_executable(dfy_plugin_module_tests plugin_module.cpp)
target_link_libraries(dfy_plugin_module_tests PRIVATE dfy_plugins dfy_build_options nlohmann_json::nlohmann_json)
target_compile_definitions(dfy_plugin_module_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(dfy_plugin_module_tests dfy_query_fixture dfy_query_example dfy_loader_dependency)

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
add_test(NAME plugins.module COMMAND dfy_plugin_module_tests "$<TARGET_FILE:dfy_query_fixture>"
  "$<TARGET_FILE:dfy_query_example>" "$<TARGET_FILE:dfy_loader_dependency>" "${CMAKE_CURRENT_BINARY_DIR}/module/$<CONFIG>")
set_tests_properties(model.config model.complete agent.session events.bus plugins.manifest plugins.registry plugins.config sdk.abi
  sdk.query_v2 sdk.handles_v2 sdk.operations_v2 sdk.notifications_v2 platform.library plugins.module PROPERTIES TIMEOUT 15)
