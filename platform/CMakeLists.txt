# 检查平台前提：当前 HTTPS 实现依赖 Windows 系统的 WinHTTP。
if(NOT WIN32)
  message(FATAL_ERROR "The current HTTPS transport requires Windows WinHTTP")
endif()

# 定义传输静态库，显式列出系统实现与公开接口。
add_library(dfy_http STATIC winhttp.cpp include/dafeiyu/http.hpp)

# 配置接口与系统依赖：公开传输头文件，内部链接 WinHTTP 并限制 Windows 宏污染。
target_include_directories(dfy_http PUBLIC include)
target_link_libraries(dfy_http PRIVATE dfy_build_options winhttp)
target_compile_definitions(dfy_http PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

# 组织 IDE 展示：保留 Platform 职责分组，源码按模块内实际目录排列。
set_target_properties(dfy_http PROPERTIES FOLDER "Platform")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/winhttp.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/http.hpp")
