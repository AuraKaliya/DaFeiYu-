# 检查平台前提：当前 HTTPS 与动态库实现依赖 Windows 系统接口。
if(NOT WIN32)
  message(FATAL_ERROR "The current HTTPS transport requires Windows WinHTTP")
endif()

# 定义有界后台输入目标，独立于模型、插件和会话。
add_library(dfy_input STATIC line_input.cpp include/dafeiyu/line_input.hpp)
target_compile_features(dfy_input PUBLIC cxx_std_20)
target_include_directories(dfy_input PUBLIC include)
target_link_libraries(dfy_input PRIVATE dfy_build_options kernel32)
target_compile_definitions(dfy_input PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(dfy_input PROPERTIES FOLDER "Platform")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/line_input.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/line_input.hpp")

# 定义受控动态库加载目标，独立于 HTTP、SDK 和插件业务状态。
add_library(dfy_library STATIC library.cpp code_module.cpp include/dafeiyu/dynamic_library.hpp include/dafeiyu/code_module.hpp)
target_compile_features(dfy_library PUBLIC cxx_std_20)
target_include_directories(dfy_library PUBLIC include)
target_link_libraries(dfy_library PRIVATE dfy_build_options kernel32)
target_compile_definitions(dfy_library PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

# 定义传输静态库，显式列出系统实现与公开接口。
add_library(dfy_http STATIC winhttp.cpp include/dafeiyu/http.hpp)

# 配置接口与系统依赖：公开传输头文件，内部链接 WinHTTP 并限制 Windows 宏污染。
target_include_directories(dfy_http PUBLIC include)
target_link_libraries(dfy_http PRIVATE dfy_build_options winhttp)
target_compile_definitions(dfy_http PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

# 组织 IDE 展示：保留 Platform 职责分组，源码按模块内实际目录排列。
set_target_properties(dfy_http dfy_library PROPERTIES FOLDER "Platform")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/winhttp.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/http.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/library.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/code_module.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/dynamic_library.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/code_module.hpp")
