# 配置测试直接复用正式模型库的读取逻辑。
dfy_test_executable(dfy_model_config_tests model_config.cpp)
target_link_libraries(dfy_model_config_tests PRIVATE dfy_model dfy_build_options nlohmann_json::nlohmann_json)
add_test(NAME model.config COMMAND dfy_model_config_tests
  "${CMAKE_CURRENT_SOURCE_DIR}/../resources/config" "${CMAKE_CURRENT_BINARY_DIR}/work/$<CONFIG>")
set_tests_properties(model.config PROPERTIES TIMEOUT 15)

dfy_add_unit_test(model.protocol dfy_model_protocol_tests model_protocol.cpp dfy_model nlohmann_json::nlohmann_json)

dfy_add_unit_test(model.messages dfy_model_messages_tests model_messages.cpp dfy_model)
dfy_add_unit_test(model.embedding_tokens dfy_embedding_token_tests embedding_tokens.cpp dfy_model)

# 验证真实后台模型线程的宿主结算、取消及退出。
dfy_add_unit_test(model.async dfy_async_model_tests async_model.cpp dfy_model)

# 单独编译协议实现，以测试传输替代 WinHTTP，测试全程不访问模型服务。
dfy_test_executable(dfy_model_client_tests model_client.cpp "../src/client.cpp"
  "../src/messages.cpp" "../src/protocol.cpp")
target_include_directories(dfy_model_client_tests PRIVATE
  ../include "$<TARGET_PROPERTY:dfy_http,INTERFACE_INCLUDE_DIRECTORIES>")
target_link_libraries(dfy_model_client_tests PRIVATE dfy_runtime dfy_build_options nlohmann_json::nlohmann_json)
target_compile_definitions(dfy_model_client_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME model.complete COMMAND dfy_model_client_tests)
set_tests_properties(model.complete PROPERTIES TIMEOUT 15)
