# 构建只依赖公开 SDK 的状态面板，并在构建目录准备可安装包。
add_library(dfy_status_example SHARED status.c)
target_link_libraries(dfy_status_example PRIVATE dfy_sdk)
target_compile_options(dfy_status_example PRIVATE "$<$<C_COMPILER_ID:MSVC>:/W4>")
target_compile_definitions(dfy_status_example PRIVATE DFY_PLUGIN_BUILD)
set_target_properties(dfy_status_example PROPERTIES FOLDER "Plugins"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/example.status")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/status.c" "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/example.status/plugin.json" INPUT "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json")
