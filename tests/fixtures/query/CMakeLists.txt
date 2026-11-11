# 构建可选择查询结果的真实 DLL，只用于 ABI 失败和引用回收测试。
add_library(dfy_query_fixture SHARED query.c)
target_link_libraries(dfy_query_fixture PRIVATE dfy_sdk)
target_compile_definitions(dfy_query_fixture PRIVATE DFY_PLUGIN_BUILD)

# 将夹具归入 Tests，保持源码的实际目录展示。
set_target_properties(dfy_query_fixture PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/query.c")
