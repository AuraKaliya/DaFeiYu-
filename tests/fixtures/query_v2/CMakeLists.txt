# 构建纯 C11 v2 查询夹具，只发布测试能力，不继承宿主字符集选项。
add_library(dfy_query_v2_fixture SHARED query.c borrow.c operation.c notification.c host.c values.c reads.c lifecycle.c nested.c arrays.c api.h)
target_link_libraries(dfy_query_v2_fixture PRIVATE dfy_sdk)
target_compile_definitions(dfy_query_v2_fixture PRIVATE DFY_PLUGIN_BUILD)
set_target_properties(dfy_query_v2_fixture PROPERTIES FOLDER "Tests")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/query.c" "${CMAKE_CURRENT_SOURCE_DIR}/borrow.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/operation.c" "${CMAKE_CURRENT_SOURCE_DIR}/notification.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/host.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/values.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/reads.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/lifecycle.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/nested.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/arrays.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/api.h")
