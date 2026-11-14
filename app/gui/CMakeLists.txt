# 建立独立验证窗口，不链接模型或插件业务库，先验收原生容器。
add_library(dfy_ui_scope STATIC ../../ui/host/frame_scope.cpp ../../ui/host/frame_scope.hpp)
target_include_directories(dfy_ui_scope PUBLIC ../../ui/host)
target_link_libraries(dfy_ui_scope PRIVATE dfy_build_options)
set_target_properties(dfy_ui_scope PROPERTIES FOLDER App)
add_executable(dfy_ui_probe WIN32 main.cpp ../paths.cpp)
target_sources(dfy_ui_probe PRIVATE interaction_probe.cpp interaction_probe.hpp)
target_sources(dfy_ui_probe PRIVATE ../../ui/host/local_content.cpp ../../ui/host/local_content.hpp ../../ui/host/webview_support.hpp)
target_sources(dfy_ui_probe PRIVATE ../../ui/host/frame_channel.cpp ../../ui/host/frame_channel.hpp)
target_sources(dfy_ui_probe PRIVATE ../../ui/host/probe_host.cpp ../../ui/host/probe_host.hpp)
target_sources(dfy_ui_probe PRIVATE ../../ui/host/browser_lifetime.cpp ../../ui/host/browser_lifetime.hpp)
target_link_libraries(dfy_ui_probe PRIVATE dfy_ui_scope dfy_webview2 dfy_build_options ole32 shell32 user32)
target_link_libraries(dfy_ui_probe PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(dfy_ui_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
set_target_properties(dfy_ui_probe PROPERTIES OUTPUT_NAME dafeiyu-ui-probe FOLDER App
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/$<CONFIG>")
# 资产改变也必须同步到运行目录，不能只在可执行文件重新链接时复制。
find_program(DFY_NPM_EXECUTABLE NAMES npm.cmd REQUIRED)
set(DFY_UI_WEB "${PROJECT_SOURCE_DIR}/ui/web")
add_custom_target(dfy_ui_assets
  COMMAND "${DFY_NPM_EXECUTABLE}" run build -- --outDir "${PROJECT_BINARY_DIR}/ui-web/$<CONFIG>"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PROJECT_BINARY_DIR}/ui-web/$<CONFIG>" "${PROJECT_BINARY_DIR}/bin/$<CONFIG>/ui"
  WORKING_DIRECTORY "${DFY_UI_WEB}" VERBATIM)
set_target_properties(dfy_ui_assets PROPERTIES FOLDER App)
add_dependencies(dfy_ui_probe dfy_ui_assets)
install(TARGETS dfy_ui_probe RUNTIME DESTINATION . CONFIGURATIONS Release)
install(DIRECTORY "${PROJECT_BINARY_DIR}/ui-web/Release/" DESTINATION ui CONFIGURATIONS Release)
install(FILES "${PROJECT_SOURCE_DIR}/docs/ui-probe-run-guide.md" DESTINATION . RENAME UI验证说明.md CONFIGURATIONS Release)
