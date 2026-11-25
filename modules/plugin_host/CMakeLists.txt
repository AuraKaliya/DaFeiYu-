add_library(dfy_plugins STATIC
  src/config.cpp src/dependencies.cpp src/host_agents.cpp src/host_arrays.cpp src/host_constraints.cpp
  src/host_enums.cpp src/host_groups.cpp src/host_mailboxes.cpp src/host_maps.cpp
  src/host_notifications.cpp src/host_processing.cpp src/host_reads.cpp src/host_routed_events.cpp
  src/host_services.cpp src/host_state.hpp src/host_subscriptions.cpp src/host_types.cpp
  src/host_unions.cpp src/host_values.cpp src/instance.cpp src/instance_hooks.cpp src/manager.cpp
  src/manifest.cpp src/module.cpp src/module_v2.cpp src/package_files.cpp src/package_files.hpp
  src/package_install.cpp src/package_remove.cpp src/plugin_order.cpp src/reader_reference.hpp
  src/registry.cpp
  include/dafeiyu/host_services.hpp include/dafeiyu/plugin_agents.hpp include/dafeiyu/plugin_config.hpp
  include/dafeiyu/plugin_dependencies.hpp include/dafeiyu/plugin_instance.hpp
  include/dafeiyu/plugin_manager.hpp include/dafeiyu/plugin_manifest.hpp
  include/dafeiyu/plugin_module.hpp include/dafeiyu/plugin_module_v2.hpp
  include/dafeiyu/plugin_order.hpp include/dafeiyu/plugin_package.hpp
  include/dafeiyu/plugin_registry.hpp include/dafeiyu/routed_event.hpp)
target_compile_features(dfy_plugins PUBLIC cxx_std_20)
target_compile_definitions(dfy_plugins PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(dfy_plugins PUBLIC include)
target_link_libraries(dfy_plugins PUBLIC dfy_library dfy_sdk dfy_runtime dfy_event_streams PRIVATE dfy_build_options dfy_digest nlohmann_json::nlohmann_json)
dfy_group_target(dfy_plugins Plugins)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
