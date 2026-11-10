add_executable(dfy_model_config_tests model_config.cpp)
target_link_libraries(dfy_model_config_tests PRIVATE dfy_model dfy_build_options nlohmann_json::nlohmann_json)
set_target_properties(dfy_model_config_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/model_config.cpp")

add_test(NAME model.config COMMAND dfy_model_config_tests
  "${PROJECT_SOURCE_DIR}/config" "${CMAKE_CURRENT_BINARY_DIR}/work/$<CONFIG>")
set_tests_properties(model.config PROPERTIES TIMEOUT 15)

# 单独编译协议实现，以测试传输替代 WinHTTP，测试全程不访问模型服务。
add_executable(dfy_model_client_tests model_client.cpp "${PROJECT_SOURCE_DIR}/model/client.cpp")
target_include_directories(dfy_model_client_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/model/include" "${PROJECT_SOURCE_DIR}/platform/include")
target_link_libraries(dfy_model_client_tests PRIVATE dfy_build_options nlohmann_json::nlohmann_json)
target_compile_definitions(dfy_model_client_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
set_target_properties(dfy_model_client_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${PROJECT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/model_client.cpp" "${PROJECT_SOURCE_DIR}/model/client.cpp")
add_test(NAME model.complete COMMAND dfy_model_client_tests)
set_tests_properties(model.complete PROPERTIES TIMEOUT 15)
