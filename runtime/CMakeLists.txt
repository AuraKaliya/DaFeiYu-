# 定义宿主运行时基础，加入已实现的域签发和类型化引用表。
add_library(dfy_runtime STATIC ids.cpp id_sequence.hpp include/dafeiyu/runtime_ids.hpp
  include/dafeiyu/handle_registry.hpp include/dafeiyu/value.hpp)
target_compile_features(dfy_runtime PUBLIC cxx_std_20)
target_include_directories(dfy_runtime PUBLIC include)
target_link_libraries(dfy_runtime PRIVATE dfy_build_options)

# 归入 Platform 职责分组，公开头文件保留实际目录。
set_target_properties(dfy_runtime PROPERTIES FOLDER "Platform")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/ids.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/id_sequence.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/runtime_ids.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/handle_registry.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/value.hpp")
