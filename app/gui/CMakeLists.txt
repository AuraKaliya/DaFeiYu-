# 建立独立验证窗口，不链接模型或插件业务库，先验收原生容器。
add_executable(dfy_ui_probe WIN32 main.cpp ../paths.cpp)
target_link_libraries(dfy_ui_probe PRIVATE dfy_webview2 dfy_build_options ole32 shell32 user32)
target_compile_definitions(dfy_ui_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
set_target_properties(dfy_ui_probe PROPERTIES OUTPUT_NAME dafeiyu-ui-probe FOLDER App
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/$<CONFIG>")
add_custom_command(TARGET dfy_ui_probe POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:dfy_ui_probe>/ui")
install(TARGETS dfy_ui_probe RUNTIME DESTINATION . CONFIGURATIONS Release)
install(DIRECTORY assets/ DESTINATION ui CONFIGURATIONS Release)
