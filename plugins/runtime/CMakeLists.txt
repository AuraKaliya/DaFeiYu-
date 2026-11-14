# 定义插件运行时静态库，包含包描述、配置及持有模块引用的 ABI 查询。
add_library(dfy_plugins STATIC manifest.cpp registry.cpp config.cpp module.cpp module_v2.cpp instance.cpp host_services.cpp host_types.cpp host_values.cpp host_arrays.cpp host_maps.cpp host_reads.cpp host_notifications.cpp host_subscriptions.cpp host_state.hpp
  include/dafeiyu/plugin_manifest.hpp include/dafeiyu/plugin_registry.hpp include/dafeiyu/plugin_config.hpp
  include/dafeiyu/plugin_module.hpp include/dafeiyu/plugin_module_v2.hpp include/dafeiyu/host_services.hpp include/dafeiyu/plugin_instance.hpp)

# 配置公开 SDK 与平台接口，JSON 解析保留在实现层，不依赖 Agent。
target_compile_features(dfy_plugins PUBLIC cxx_std_20)
target_sources(dfy_plugins PRIVATE host_enums.cpp host_constraints.cpp host_unions.cpp)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/host_enums.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/host_constraints.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/host_unions.cpp")
target_include_directories(dfy_plugins PUBLIC include)
target_link_libraries(dfy_plugins PUBLIC dfy_library dfy_sdk dfy_runtime dfy_event_streams PRIVATE dfy_build_options nlohmann_json::nlohmann_json)

# 组织 IDE 展示：按 Plugins 职责分组，源码保持模块内实际目录。
set_target_properties(dfy_plugins PROPERTIES FOLDER "Plugins")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/manifest.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/registry.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/config.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/module.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/module_v2.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/instance.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/plugin_instance.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/host_services.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/host_state.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/host_types.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/host_values.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/host_arrays.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/host_maps.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/host_reads.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/host_notifications.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/host_subscriptions.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/host_services.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/plugin_manifest.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/plugin_registry.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/plugin_config.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/plugin_module.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/plugin_module_v2.hpp")
