add_library(dfy_runtime STATIC
  src/array_value.cpp src/budget.cpp src/completion_notifications.cpp src/descriptor_name.hpp
  src/descriptor_validation.cpp src/descriptor_validation.hpp src/element_descriptor.cpp
  src/enum_descriptor.cpp src/enum_value.cpp src/field_validation.cpp src/field_validation.hpp
  src/fifo_executor.cpp src/id_sequence.hpp src/ids.cpp src/map_value.cpp src/operation.cpp
  src/owner_scheduler.cpp src/resource_owner.cpp src/result_group.cpp src/retry_controller.cpp
  src/serial_domain.cpp src/serial_wait_graph.cpp src/struct_descriptor.cpp src/struct_value.cpp
  src/type_catalog.cpp src/union_descriptor.cpp src/union_value.cpp src/utf8.cpp src/value.cpp
  include/dafeiyu/array_value.hpp include/dafeiyu/budget.hpp
  include/dafeiyu/completion_notifications.hpp include/dafeiyu/element_descriptor.hpp
  include/dafeiyu/enum_descriptor.hpp include/dafeiyu/enum_value.hpp include/dafeiyu/fifo_executor.hpp
  include/dafeiyu/handle_registry.hpp include/dafeiyu/map_value.hpp include/dafeiyu/operation.hpp
  include/dafeiyu/owner_scheduler.hpp include/dafeiyu/resource_owner.hpp
  include/dafeiyu/result_group.hpp include/dafeiyu/retry_controller.hpp include/dafeiyu/runtime_ids.hpp
  include/dafeiyu/serial_domain.hpp include/dafeiyu/serial_wait_graph.hpp
  include/dafeiyu/struct_descriptor.hpp include/dafeiyu/struct_value.hpp
  include/dafeiyu/type_catalog.hpp include/dafeiyu/union_descriptor.hpp include/dafeiyu/union_value.hpp
  include/dafeiyu/utf8.hpp include/dafeiyu/value.hpp)
target_compile_features(dfy_runtime PUBLIC cxx_std_20)
target_include_directories(dfy_runtime PUBLIC include)
target_link_libraries(dfy_runtime PRIVATE dfy_build_options)
dfy_group_target(dfy_runtime Platform)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
