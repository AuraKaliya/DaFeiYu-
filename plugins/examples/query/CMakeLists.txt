# 使用纯 C 编译最小查询 DLL，只依赖 SDK，不链接宿主实现或继承其字符集选项。
add_library(dfy_query_example SHARED query.c)
target_link_libraries(dfy_query_example PRIVATE dfy_sdk)
target_compile_definitions(dfy_query_example PRIVATE DFY_PLUGIN_BUILD)

# 将实际示例目标归入 Plugins，源码保持模块内目录。
set_target_properties(dfy_query_example PROPERTIES FOLDER "Plugins")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/query.c")
