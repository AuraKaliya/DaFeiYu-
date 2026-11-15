# 独立 C11 工具生产方，仅使用公开 SDK，不依赖宿主 C++ 类型。
add_library(dfy_tools_fixture SHARED tools.c)
target_link_libraries(dfy_tools_fixture PRIVATE dfy_sdk)
target_compile_features(dfy_tools_fixture PRIVATE c_std_11)
target_compile_definitions(dfy_tools_fixture PRIVATE DFY_PLUGIN_BUILD)
set_target_properties(dfy_tools_fixture PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/tools.c")
