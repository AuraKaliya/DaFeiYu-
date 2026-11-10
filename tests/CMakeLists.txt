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

# 组织 IDE 展示：测试自身源码按本目录排列，借用的业务源码保留来源模块路径。
set_target_properties(dfy_model_config_tests dfy_model_client_tests dfy_agent_tests dfy_events_tests
  PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/model_config.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/model_client.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/agent_session.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/events.cpp")
source_group(TREE "${PROJECT_SOURCE_DIR}" FILES
  "${PROJECT_SOURCE_DIR}/model/client.cpp" "${PROJECT_SOURCE_DIR}/agent/session.cpp"
  "${PROJECT_SOURCE_DIR}/app/chat.cpp")

# 注册 CTest：配置测试使用隔离工作目录，全部测试统一设置执行时限。
add_test(NAME model.config COMMAND dfy_model_config_tests
  "${PROJECT_SOURCE_DIR}/config" "${CMAKE_CURRENT_BINARY_DIR}/work/$<CONFIG>")
add_test(NAME model.complete COMMAND dfy_model_client_tests)
add_test(NAME agent.session COMMAND dfy_agent_tests)
add_test(NAME events.bus COMMAND dfy_events_tests)
set_tests_properties(model.config model.complete agent.session events.bus PROPERTIES TIMEOUT 15)
