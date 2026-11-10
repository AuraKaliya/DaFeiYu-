add_executable(dfy_model_config_tests model_config.cpp)
target_link_libraries(dfy_model_config_tests PRIVATE dfy_model dfy_build_options nlohmann_json::nlohmann_json)
set_target_properties(dfy_model_config_tests PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/model_config.cpp")

add_test(NAME model.config COMMAND dfy_model_config_tests
  "${PROJECT_SOURCE_DIR}/config" "${CMAKE_CURRENT_BINARY_DIR}/work/$<CONFIG>")
set_tests_properties(model.config PROPERTIES TIMEOUT 15)
