# 定义宿主运行时基础，加入已实现的数据能力和有界执行器。
add_library(dfy_runtime STATIC ids.cpp value.cpp array_value.cpp map_value.cpp struct_descriptor.cpp type_catalog.cpp struct_value.cpp fifo_executor.cpp operation.cpp
  completion_notifications.cpp resource_owner.cpp id_sequence.hpp include/dafeiyu/runtime_ids.hpp
  include/dafeiyu/handle_registry.hpp include/dafeiyu/value.hpp include/dafeiyu/struct_descriptor.hpp include/dafeiyu/type_catalog.hpp
  include/dafeiyu/struct_value.hpp include/dafeiyu/fifo_executor.hpp include/dafeiyu/operation.hpp
  include/dafeiyu/completion_notifications.hpp include/dafeiyu/resource_owner.hpp include/dafeiyu/array_value.hpp include/dafeiyu/map_value.hpp)
target_compile_features(dfy_runtime PUBLIC cxx_std_20)
target_sources(dfy_runtime PRIVATE enum_descriptor.cpp enum_value.cpp descriptor_name.hpp
  include/dafeiyu/enum_descriptor.hpp include/dafeiyu/enum_value.hpp)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/enum_descriptor.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/descriptor_name.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/enum_value.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/enum_value.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/enum_descriptor.hpp")
target_include_directories(dfy_runtime PUBLIC include)
target_link_libraries(dfy_runtime PRIVATE dfy_build_options)

# 归入 Platform 职责分组，公开头文件保留实际目录。
set_target_properties(dfy_runtime PROPERTIES FOLDER "Platform")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/ids.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/id_sequence.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/value.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/array_value.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/array_value.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/map_value.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/map_value.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/struct_descriptor.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/type_catalog.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/struct_value.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/fifo_executor.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/operation.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/completion_notifications.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/resource_owner.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/runtime_ids.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/handle_registry.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/value.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/struct_descriptor.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/type_catalog.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/struct_value.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/fifo_executor.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/operation.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/completion_notifications.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/resource_owner.hpp")
