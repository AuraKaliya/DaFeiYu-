# 在独立适配层连接原生插件和 Agent，插件运行时本身保持不依赖 Agent。
add_library(dfy_plugin_tools STATIC native_work.cpp include/dafeiyu/plugin_tool_work.hpp)
target_sources(dfy_plugin_tools PRIVATE companion.cpp include/dafeiyu/plugin_companion.hpp)
target_sources(dfy_plugin_tools PRIVATE control_hooks.cpp include/dafeiyu/plugin_hooks.hpp)
target_include_directories(dfy_plugin_tools PUBLIC include)
target_sources(dfy_plugin_tools PRIVATE registration.cpp include/dafeiyu/plugin_tools.hpp)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/registration.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/plugin_tools.hpp")
target_link_libraries(dfy_plugin_tools PUBLIC dfy_plugins dfy_agent PRIVATE dfy_build_options)
set_target_properties(dfy_plugin_tools PROPERTIES FOLDER "Plugins")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/native_work.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/plugin_tool_work.hpp")
