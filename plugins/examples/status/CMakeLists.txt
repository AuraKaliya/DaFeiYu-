# 根工程与独立 SDK 使用同一状态面板构建入口。
cmake_minimum_required(VERSION 3.28)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(dafeiyu_status_sdk LANGUAGES C)
  if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The status panel SDK example targets Windows x64")
  endif()
  add_library(dfy_sdk INTERFACE)
  target_include_directories(dfy_sdk INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
endif()

# 构建只依赖公开 SDK 的状态面板，并在构建目录准备可安装包。
add_library(dfy_status_example SHARED status.c)
target_link_libraries(dfy_status_example PRIVATE dfy_sdk)
target_compile_options(dfy_status_example PRIVATE "$<$<C_COMPILER_ID:MSVC>:/W4>")
target_compile_definitions(dfy_status_example PRIVATE DFY_PLUGIN_BUILD)
set_target_properties(dfy_status_example PROPERTIES FOLDER "Plugins"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/example.status")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/status.c" "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/example.status/plugin.json" INPUT "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json")

# 源码与包资源由示例维护，同时供运行包和独立 SDK 消费者使用。
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  install(TARGETS dfy_status_example RUNTIME DESTINATION examples/plugins/example.status CONFIGURATIONS Release COMPONENT Runtime)
  install(FILES plugin.json DESTINATION examples/plugins/example.status CONFIGURATIONS Release COMPONENT Runtime)
  install(FILES CMakeLists.txt status.c plugin.json
    DESTINATION sdk/examples/status CONFIGURATIONS Release COMPONENT SDK)
endif()
