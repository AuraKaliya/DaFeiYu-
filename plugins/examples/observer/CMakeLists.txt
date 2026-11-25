cmake_minimum_required(VERSION 3.28)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(dafeiyu_observer_sdk LANGUAGES C)
  if(NOT WIN32)
    message(FATAL_ERROR "This SDK example targets Windows native DLLs")
  endif()
  add_library(dfy_sdk INTERFACE)
  target_include_directories(dfy_sdk INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
endif()

# 独立纯 C11 观察插件只依赖公开 SDK，本体不链接它的实现。
add_library(dfy_observer SHARED observer.c observer_api.h)
target_link_libraries(dfy_observer PRIVATE dfy_sdk)
target_compile_definitions(dfy_observer PRIVATE DFY_PLUGIN_BUILD)
target_compile_options(dfy_observer PRIVATE "$<$<C_COMPILER_ID:MSVC>:/W4>")
set_target_properties(dfy_observer PROPERTIES FOLDER "Plugins"
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/example.observer")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/observer.c" "${CMAKE_CURRENT_SOURCE_DIR}/observer_api.h")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/example.observer/plugin.json" INPUT "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/example.observer/defaults.json" INPUT "${CMAKE_CURRENT_SOURCE_DIR}/defaults.json")

# 源码与包资源由示例维护，同时供运行包和独立 SDK 消费者使用。
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  install(TARGETS dfy_observer RUNTIME DESTINATION examples/plugins/example.observer CONFIGURATIONS Release COMPONENT Runtime)
  install(FILES plugin.json defaults.json DESTINATION examples/plugins/example.observer CONFIGURATIONS Release COMPONENT Runtime)
  install(FILES CMakeLists.txt observer.c observer_api.h plugin.json defaults.json
    DESTINATION sdk/examples/observer CONFIGURATIONS Release COMPONENT SDK)
endif()
