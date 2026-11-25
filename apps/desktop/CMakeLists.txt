include("${PROJECT_SOURCE_DIR}/cmake/WebView2.cmake")
add_library(dfy_ui_scope STATIC src/host/frame_scope.cpp src/host/frame_scope.hpp)
target_include_directories(dfy_ui_scope PUBLIC src/host)
target_link_libraries(dfy_ui_scope PRIVATE dfy_build_options)
dfy_group_target(dfy_ui_scope App)

add_executable(dafeiyu WIN32 src/main.cpp src/interaction_probe.cpp src/interaction_probe.hpp
  src/host/local_content.cpp src/host/local_content.hpp src/host/webview_support.hpp
  src/host/frame_channel.cpp src/host/frame_channel.hpp src/host/probe_host.cpp src/host/probe_host.hpp
  src/host/browser_lifetime.cpp src/host/browser_lifetime.hpp)
target_link_libraries(dafeiyu PRIVATE dfy_ui_scope dfy_resident_tray dfy_webview2 dfy_build_options
  dfy_management nlohmann_json::nlohmann_json ole32 shell32 user32 dwmapi)
target_compile_definitions(dafeiyu PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
set_target_properties(dafeiyu PROPERTIES FOLDER App
  VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:dafeiyu>"
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/$<CONFIG>")
# 运行和安装使用同一份前端产物，Vite 每次重建专用资产目录以移除已退休的文件。
find_program(DFY_NPM_EXECUTABLE NAMES npm.cmd REQUIRED)
set(DFY_UI_WEB "${CMAKE_CURRENT_SOURCE_DIR}/web")
add_custom_target(dfy_ui_assets
  COMMAND "${CMAKE_COMMAND}" "-DWEB_DIR=${DFY_UI_WEB}" "-DNPM=${DFY_NPM_EXECUTABLE}"
    "-DASSET_DIR=${PROJECT_BINARY_DIR}/bin/$<CONFIG>/ui" "-DLOCK_FILE=${PROJECT_SOURCE_DIR}/build/desktop-web.lock"
    -P "${PROJECT_SOURCE_DIR}/cmake/BuildDesktopWeb.cmake" VERBATIM)
set_target_properties(dfy_ui_assets PROPERTIES FOLDER App)
add_dependencies(dafeiyu dfy_ui_assets dafeiyu-host)
install(TARGETS dafeiyu RUNTIME DESTINATION . CONFIGURATIONS Release COMPONENT Runtime)
install(DIRECTORY "${PROJECT_BINARY_DIR}/bin/Release/ui/" DESTINATION ui CONFIGURATIONS Release COMPONENT Runtime)
install(FILES "${PROJECT_SOURCE_DIR}/docs/ui-probe-run-guide.md" DESTINATION . RENAME UI验证说明.md CONFIGURATIONS Release COMPONENT Runtime)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
