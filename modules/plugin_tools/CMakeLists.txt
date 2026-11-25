add_library(dfy_plugin_tools STATIC
  src/companion.cpp src/control_hooks.cpp src/native_work.cpp src/registration.cpp
  include/dafeiyu/plugin_companion.hpp include/dafeiyu/plugin_hooks.hpp
  include/dafeiyu/plugin_tool_work.hpp include/dafeiyu/plugin_tools.hpp)
target_include_directories(dfy_plugin_tools PUBLIC include)
target_link_libraries(dfy_plugin_tools PUBLIC dfy_plugins dfy_agent PRIVATE dfy_build_options)
dfy_group_target(dfy_plugin_tools Plugins)
