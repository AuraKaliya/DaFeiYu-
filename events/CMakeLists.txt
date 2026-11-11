# 定义独立事件静态库，显式列出分发实现与公开接口。
add_library(dfy_events STATIC bus.cpp include/dafeiyu/events.hpp)

# 配置接口与编译要求：公开头文件和 C++20 要求，仅引入自有代码编译选项。
target_compile_features(dfy_events PUBLIC cxx_std_20)
target_include_directories(dfy_events PUBLIC include)
target_link_libraries(dfy_events PRIVATE dfy_build_options)

# 组织 IDE 展示：保留 Platform 职责分组，源码按模块内实际目录排列。
set_target_properties(dfy_events PROPERTIES FOLDER "Platform")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/bus.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/events.hpp")
