# 定义离线测试目标与依赖；配置测试直接复用正式模型库的读取逻辑。
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

# 组织 IDE 展示：测试自身源码按本目录排列，借用的业务源码保留来源模块路径。
set_target_properties(dfy_model_config_tests dfy_model_client_tests dfy_agent_tests dfy_events_tests dfy_plugin_manifest_tests
  dfy_plugin_registry_tests dfy_plugin_config_tests dfy_sdk_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/model_config.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/model_client.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/agent_session.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/events.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/plugin_manifest.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/plugin_registry.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/plugin_config.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/sdk_abi.cpp")
source_group(TREE "${PROJECT_SOURCE_DIR}" FILES
  "${PROJECT_SOURCE_DIR}/model/client.cpp" "${PROJECT_SOURCE_DIR}/agent/session.cpp"
  "${PROJECT_SOURCE_DIR}/app/chat.cpp")

# 注册 CTest：配置测试使用隔离工作目录，全部测试统一设置执行时限。
add_test(NAME model.config COMMAND dfy_model_config_tests
  "${PROJECT_SOURCE_DIR}/config" "${CMAKE_CURRENT_BINARY_DIR}/work/$<CONFIG>")
add_test(NAME model.complete COMMAND dfy_model_client_tests)
add_test(NAME agent.session COMMAND dfy_agent_tests)
add_test(NAME events.bus COMMAND dfy_events_tests)
add_test(NAME plugins.manifest COMMAND dfy_plugin_manifest_tests "${CMAKE_CURRENT_BINARY_DIR}/manifest/$<CONFIG>")
add_test(NAME plugins.registry COMMAND dfy_plugin_registry_tests "${CMAKE_CURRENT_BINARY_DIR}/registry/$<CONFIG>")
add_test(NAME plugins.config COMMAND dfy_plugin_config_tests "${CMAKE_CURRENT_BINARY_DIR}/plugin-config/$<CONFIG>")
add_test(NAME sdk.abi COMMAND dfy_sdk_tests "$<TARGET_FILE:dfy_query_example>")
set_tests_properties(model.config model.complete agent.session events.bus plugins.manifest plugins.registry plugins.config sdk.abi
  PROPERTIES TIMEOUT 15)
