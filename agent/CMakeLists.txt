# 定义会话静态库，显式列出实现与公开接口。
add_library(dfy_agent STATIC session.cpp include/dafeiyu/session.hpp)

# 配置接口依赖：向使用者公开会话头文件及其依赖的模型接口。
target_include_directories(dfy_agent PUBLIC include)
target_link_libraries(dfy_agent PUBLIC dfy_model PRIVATE dfy_build_options)

# 组织 IDE 展示：保留 Agent 职责分组，源码按模块内实际目录排列。
set_target_properties(dfy_agent PROPERTIES FOLDER "Agent")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/session.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/session.hpp")
