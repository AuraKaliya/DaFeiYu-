# 独立纯 C11 观察插件只依赖公开 SDK，本体不链接它的实现。
add_library(dfy_observer SHARED observer.c observer_api.h)
target_link_libraries(dfy_observer PRIVATE dfy_sdk)
target_compile_definitions(dfy_observer PRIVATE DFY_PLUGIN_BUILD)
target_compile_options(dfy_observer PRIVATE "$<$<C_COMPILER_ID:MSVC>:/W4>")
set_target_properties(dfy_observer PROPERTIES FOLDER "Plugins"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/example.observer")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/observer.c" "${CMAKE_CURRENT_SOURCE_DIR}/observer_api.h")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/example.observer/plugin.json" INPUT "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/example.observer/defaults.json" INPUT "${CMAKE_CURRENT_SOURCE_DIR}/defaults.json")
