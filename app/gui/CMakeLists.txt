# 建立独立验证窗口，不链接模型或插件业务库，先验收原生容器。
add_library(dfy_ui_scope STATIC ../../ui/host/frame_scope.cpp ../../ui/host/frame_scope.hpp)
target_include_directories(dfy_ui_scope PUBLIC ../../ui/host)
target_link_libraries(dfy_ui_scope PRIVATE dfy_build_options)
set_target_properties(dfy_ui_scope PROPERTIES FOLDER App)
add_executable(dfy_ui_probe WIN32 main.cpp ../paths.cpp)
target_link_libraries(dfy_ui_probe PRIVATE dfy_ui_scope dfy_webview2 dfy_build_options ole32 shell32 user32)
target_compile_definitions(dfy_ui_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
set_target_properties(dfy_ui_probe PROPERTIES OUTPUT_NAME dafeiyu-ui-probe FOLDER App
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/$<CONFIG>")
add_custom_command(TARGET dfy_ui_probe POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:dfy_ui_probe>/ui")
install(TARGETS dfy_ui_probe RUNTIME DESTINATION . CONFIGURATIONS Release)
install(DIRECTORY assets/ DESTINATION ui CONFIGURATIONS Release)
