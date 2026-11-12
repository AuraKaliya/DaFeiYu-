# 构建独立 v2 故障 DLL，验证生产加载器的布局与失败回收。
add_library(dfy_query_v2_faults SHARED query.c)
target_link_libraries(dfy_query_v2_faults PRIVATE dfy_sdk)
target_compile_definitions(dfy_query_v2_faults PRIVATE DFY_PLUGIN_BUILD)
set_target_properties(dfy_query_v2_faults PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES "${CMAKE_CURRENT_SOURCE_DIR}/query.c")
