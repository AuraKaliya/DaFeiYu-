# 共享自有目标的警告级别与 MSVC 源码字符集。
add_library(dfy_build_options INTERFACE)
target_compile_options(dfy_build_options INTERFACE
  "$<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive-;/utf-8>"
  "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic>")

# 每个 C++ 目标的编译进程与构建预设调度的项目数分别设限。
if(MSVC)
  set(DFY_MSVC_COMPILE_JOBS 2 CACHE STRING "Maximum MSVC compiler processes per C++ target")
  if(NOT DFY_MSVC_COMPILE_JOBS MATCHES "^[1-9][0-9]*$" OR DFY_MSVC_COMPILE_JOBS GREATER 65536)
    message(FATAL_ERROR "DFY_MSVC_COMPILE_JOBS must be an integer from 1 to 65536")
  endif()
  # VS 扫描 C++20 模块依赖时可以逐文件调度，其进程数由 MSBuild 单独读取。
  if(CMAKE_GENERATOR MATCHES "^Visual Studio")
    list(APPEND CMAKE_VS_GLOBALS "MultiProcMaxCount=${DFY_MSVC_COMPILE_JOBS}")
  endif()
  target_compile_options(dfy_build_options INTERFACE
    "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/MP${DFY_MSVC_COMPILE_JOBS}>")
endif()

# 按目标所属职责设置 IDE 分组，以模块内声明的文件树展示源码。
function(dfy_group_target target folder)
  set_property(TARGET ${target} PROPERTY FOLDER "${folder}")
  get_target_property(sources ${target} SOURCES)
  source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${sources})
endfunction()
