# 定义插件运行时静态库，包含清单、目录发现和独立配置快照。
add_library(dfy_plugins STATIC manifest.cpp registry.cpp config.cpp
  include/dafeiyu/plugin_manifest.hpp include/dafeiyu/plugin_registry.hpp include/dafeiyu/plugin_config.hpp)

# 配置公开接口与内部解析依赖，不链接 Agent 或 DLL 加载器。
target_compile_features(dfy_plugins PUBLIC cxx_std_20)
target_include_directories(dfy_plugins PUBLIC include)
target_link_libraries(dfy_plugins PRIVATE dfy_build_options nlohmann_json::nlohmann_json)

# 组织 IDE 展示：按 Plugins 职责分组，源码保持模块内实际目录。
set_target_properties(dfy_plugins PROPERTIES FOLDER "Plugins")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/manifest.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/registry.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/config.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/plugin_manifest.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/plugin_registry.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/dafeiyu/plugin_config.hpp")
